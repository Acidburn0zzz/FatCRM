#include "salesforceresource.h"

#include "contactshandler.h"
#include "settings.h"
#include "settingsadaptor.h"
#include "salesforceconfigdialog.h"
#include "salesforcesoap.h"

#include <akonadi/changerecorder.h>
#include <akonadi/collection.h>
#include <akonadi/itemfetchscope.h>

#include <KLocale>
#include <KWindowSystem>

#include <KDSoapMessage.h>

#include <QtDBus/QDBusConnection>

using namespace Akonadi;

static QString endPointFromHostString( const QString &host )
{
    KUrl url( host );
    url.setFileName( QLatin1String( "soap.php" ) );
    url.setQuery( QString() );

    return url.url();
}

static QString nameFromHostString( const QString &host )
{
    KUrl url( host );

    return i18nc("@title user visible resource identifier, including host name", "Salesforce on %1", url.host() );
}

SalesforceResource::SalesforceResource( const QString &id )
    : ResourceBase( id ),
      mSoap( new SforceService ),
      mModuleHandlers( new ModuleHandlerHash )
{
    new SettingsAdaptor( Settings::self() );
    QDBusConnection::sessionBus().registerObject( QLatin1String( "/Settings" ),
                                                  Settings::self(), QDBusConnection::ExportAdaptors );

    setNeedsNetwork( true );

    // make sure itemAdded() and itemChanged() get the full item from Akonadi before being called
    changeRecorder()->itemFetchScope().fetchFullPayload();

    // make sure these call have the collection available as well
    changeRecorder()->fetchCollection( true );

    connectSoapProxy();

#if 0
    mSoap->setEndPoint( endPointFromHostString( Settings::self()->host() ) );
#endif
    setName( Settings::self()->user() + QLatin1Char( '@' ) + nameFromHostString( Settings::self()->host() ) );
}

SalesforceResource::~SalesforceResource()
{
    qDeleteAll( *mModuleHandlers );
    delete mModuleHandlers;
    delete mSoap;
}

void SalesforceResource::configure( WId windowId )
{
    SalesforceConfigDialog dialog( Settings::self() );

    // make sure we are seen as a child window of the caller's window
    // otherwise focus stealing prevention might put us behind it
    KWindowSystem::setMainWindow( &dialog, windowId );

    int result = dialog.exec();

    if ( result == QDialog::Rejected ) {
        emit configurationDialogRejected();
        return;
    }

    const QString host = dialog.host();
    const QString user = dialog.user();
    const QString password = dialog.password();

    bool newLogin = false;

    // change of host requires new instance of the SOAP client as its setEndPoint() method
    // does not change the internal client interface which actually handles the communication
    if ( host != Settings::self()->host() ) {
        if ( !mSessionId.isEmpty() ) {
            mSoap->logout();
            mSessionId = QString();
        }

        mSoap->disconnect();
        mSoap->deleteLater();

        setName( user + QLatin1Char( '@' ) + nameFromHostString( host ) );

        mSoap = new SforceService;
#if 0
        mSoap->setEndPoint( endPointFromHostString( host ) );
#endif
        connectSoapProxy();

        newLogin = true;
    }

    if ( user != Settings::self()->user() || password != Settings::self()->password() ) {
        if ( !mSessionId.isEmpty() ) {
            mSoap->logout();
            mSessionId = QString();
        }

        setName( user + QLatin1Char( '@' ) + nameFromHostString( host ) );

        newLogin = true;
    }

    Settings::self()->setHost( host );
    Settings::self()->setUser( user );
    Settings::self()->setPassword( password );
    Settings::self()->writeConfig();

    emit configurationDialogAccepted();

    if ( newLogin && isOnline() ) {
        doLogin();
    }
}

void SalesforceResource::aboutToQuit()
{
    if ( !mSessionId.isEmpty() ) {
        // just a curtesy to the server
        mSoap->asyncLogout();
    }
}

void SalesforceResource::doSetOnline( bool online )
{
    ResourceBase::doSetOnline( online );

    if ( online ) {
        if ( Settings::self()->host().isEmpty() ) {
            const QString message = i18nc( "@status", "No server configured" );
            status( Broken, message );
            error( message );
        } else if ( Settings::self()->user().isEmpty() ) {
            const QString message = i18nc( "@status", "No user name configured" );
            status( Broken, message );
            error( message );
        } else {
            doLogin();
        }
    }
}

void SalesforceResource::doLogin()
{
    const QString username = Settings::self()->user();
    const QString password = Settings::self()->password();

    TNS__Login userAuth;
    userAuth.setUsername( username );
    userAuth.setPassword( password );

    mSessionId = QString();

    // results handled by slots loginDone() and loginError()
    mSoap->asyncLogin( userAuth );
}

void SalesforceResource::itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection )
{
    // TODO check if mSessionId is valid?

    QString message;

    // find the handler for the module represented by the given collection and let it
    // perform the respective "set entry" operation
    ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( collection.remoteId() );
    if ( moduleIt != mModuleHandlers->constEnd() ) {
        // save item so we can reference it in the result slots
        mPendingItem = item;

        // results handled by slots setEntryDone() and setEntryError()
        if ( !moduleIt.value()->setEntry( item, mSoap ) ) {
            mPendingItem = Item();
            message = i18nc( "@status", "Attempting to add malformed item to folder %1", collection.name() );
        }
    } else {
        message = i18nc( "@status", "Cannot add items to folder %1", collection.name() );
    }

    if ( message.isEmpty() ) {
        status( Running );
    } else {
        status( Broken, message );
        error( message );
        cancelTask( message );
    }
}

void SalesforceResource::itemChanged( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
    // TODO maybe we can use parts to get only a subset of fields in ModuleHandler::setEntry()
    Q_UNUSED( parts );

    // TODO check if mSessionId is valid?
    QString message;

    // find the handler for the module represented by the given collection and let it
    // perform the respective "set entry" operation
    const Collection collection = item.parentCollection();
    ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( collection.remoteId() );
    if ( moduleIt != mModuleHandlers->constEnd() ) {
        // save item so we can reference it in the result slots
        mPendingItem = item;

        // results handled by slots setEntryDone() and setEntryError()
        if ( !moduleIt.value()->setEntry( item, mSoap ) ) {
            mPendingItem = Item();
            message = i18nc( "@status", "Attempting to modify a malformed item in folder %1", collection.name() );
        }
    } else {
        message = i18nc( "@status", "Cannot modify items in folder %1", collection.name() );
    }

    if ( message.isEmpty() ) {
        status( Running );
    } else {
        status( Broken, message );
        error( message );
        cancelTask( message );
    }
}

void SalesforceResource::itemRemoved( const Akonadi::Item &item )
{
    const Collection collection = item.parentCollection();

    // not uploaded yet?
    if ( item.remoteId().isEmpty() || collection.remoteId().isEmpty() ) {
        changeCommitted( item );
        return;
    }

    // TODO check if mSessionId is valid?

    // delete just required identifiers
    // no need for type specific code
    TNS__ID idField;
    idField.setValue( item.remoteId() );

    TNS__Delete deleteParams;
    deleteParams.setIds( QList<TNS__ID>() << idField );

    // save item so we can reference it in the result slots
    mPendingItem = item;

    // results handled by slots getEntryDone() and getEntryError()
    mSoap->asyncDelete( deleteParams );

    status( Running );
}

void SalesforceResource::connectSoapProxy()
{
    Q_ASSERT( mSoap != 0 );

    connect( mSoap, SIGNAL( loginDone( TNS__LoginResponse ) ), this, SLOT( loginDone( TNS__LoginResponse ) ) );
    connect( mSoap, SIGNAL( loginError( KDSoapMessage ) ), this, SLOT( loginError( KDSoapMessage ) ) );
#if 0
    connect( mSoap, SIGNAL( get_available_modulesDone( TNS__Module_list ) ),
             this,  SLOT( getAvailableModulesDone( TNS__Module_list ) ) );
    connect( mSoap, SIGNAL( get_available_modulesError( KDSoapMessage ) ),
             this,  SLOT( getAvailableModulesError( KDSoapMessage ) ) );
#endif

    connect( mSoap, SIGNAL( queryDone( TNS__QueryResponse ) ),
             this,  SLOT( getEntryListDone( TNS__QueryResponse ) ) );
    connect( mSoap, SIGNAL( queryError( KDSoapMessage ) ),
             this,  SLOT( getEntryListError( KDSoapMessage ) ) );
    connect( mSoap, SIGNAL( queryMoreDone( TNS__QueryMoreResponse ) ),
             this,  SLOT( getEntryListDone( TNS__QueryMoreResponse ) ) );
    connect( mSoap, SIGNAL( queryMoreError( KDSoapMessage ) ),
             this,  SLOT( getEntryListError( KDSoapMessage ) ) );

    connect( mSoap, SIGNAL( upsertDone( TNS__UpsertResponse ) ),
             this,  SLOT( setEntryDone( TNS__UpsertResponse ) ) );
    connect( mSoap, SIGNAL( upsertError( KDSoapMessage ) ),
             this,  SLOT( setEntryError( KDSoapMessage ) ) );
}

void SalesforceResource::retrieveCollections()
{
    // TODO could attempt automatic login
    if ( mSessionId.isEmpty() ) {
        QString message;
        if ( Settings::host().isEmpty() ) {
#if 0
            message = i18nc( "@status", "No server configured" );
#endif
        } else if ( Settings::self()->user().isEmpty() ) {
            message = i18nc( "@status", "No user name configured" );
        } else {
            message = i18nc( "@status", "Unable to login to %1", Settings::host() );
        }

        status( Broken, message );
        error( message );
        cancelTask( message );
    } else {
        status( Running, i18nc( "@status", "Retrieving folders" ) );
        // results handled by slots getAvailableModulesDone() and getAvailableModulesError()
#if 0
        mSoap->asyncGet_available_modules( mSessionId );
#else
        Collection topLevelCollection;
        topLevelCollection.setRemoteId( identifier() );
        topLevelCollection.setName( name() );
        topLevelCollection.setParentCollection( Collection::root() );

        // Our top level collection only contains other collections (no items) and cannot be
        // modified by clients
        topLevelCollection.setContentMimeTypes( QStringList() << Collection::mimeType() );
        topLevelCollection.setRights( Collection::ReadOnly );

        Collection::List collections;
        collections << topLevelCollection;

        // create just ContactsHandler
        ModuleHandler* handler = new ContactsHandler;
        mModuleHandlers->insert( handler->moduleName(), handler );

        Collection collection = handler->collection();

        collection.setParentCollection( topLevelCollection );
        collections << collection;
#endif
    }
}

void SalesforceResource::retrieveItems( const Akonadi::Collection &collection )
{
    if ( collection.parentCollection() == Collection::root() ) {
        itemsRetrieved( Item::List() );
        return;
    }

    // TODO could attempt automatic login
    if ( mSessionId.isEmpty() ) {
        QString message;
        if ( Settings::host().isEmpty() ) {
            message = i18nc( "@status", "No server configured" );
        } else if ( Settings::self()->user().isEmpty() ) {
            message = i18nc( "@status", "No user name configured" );
        } else {
            message = i18nc( "@status", "Unable to login to %1", Settings::host() );
        }

        status( Broken, message );
        error( message );
        cancelTask( message );
    } else {
        // find the handler for the module represented by the given collection and let it
        // perform the respective "list entries" operation
        ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( collection.remoteId() );
        if ( moduleIt != mModuleHandlers->constEnd() ) {
            status( Running, i18nc( "@status", "Retrieving contents of folder %1", collection.name() ) );

            // getting items in batches
            setItemStreamingEnabled( true );

            // results handled by slots getEntryListDone() and getEntryListError()

            TNS__QueryLocator locator;
            moduleIt.value()->listEntries( locator, mSoap );
        } else {
            kDebug() << "No module handler for collection" << collection;
            itemsRetrieved( Item::List() );
        }
    }
}

bool SalesforceResource::retrieveItem( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
    Q_UNUSED( item );
    Q_UNUSED( parts );

    // TODO not implemented yet
    // retrieveItems() provides full items anyway so this one is not called
    // (no need for getting additional data)
    // should be implemented for consistency though

    return true;
}

void SalesforceResource::loginDone( const TNS__LoginResponse &callResult )
{
    const TNS__LoginResult loginResult = callResult.result();

    QString message;
    mSessionId = QString();

    const QString sessionId = loginResult.sessionId();
    if ( sessionId.isEmpty() ) {
        message = i18nc( "@status", "Login failed: server returned an empty session identifier" );
    } else if ( mSessionId == QLatin1String( "-1" ) ) {
        message = i18nc( "@status", "Login failed: server returned an invalid session identifier" );
    } else {
        mSessionId = sessionId;
        kDebug() << "Login succeeded: sessionId=" << mSessionId;
    }

    if ( message.isEmpty() ) {
        // salesforce might issue a redirect on login so set a new endpoint
        delete mSoap;
        mSoap = new SforceService;
        mSoap->setEndPoint( loginResult.serverUrl() );
        connectSoapProxy();

        status( Idle );

#if 0
        synchronizeCollectionTree();
#else
        const TNS__DescribeGlobalResponse callResponse = mSoap->describeGlobal();
        const TNS__DescribeGlobalResult result = callResponse.result();
        kDebug() << "describeGlobal: maxBatchSize=" << result.maxBatchSize()
                 << "encoding=" << result.encoding();
        const QList<TNS__DescribeGlobalSObjectResult> sobjects = result.sobjects();
        Q_FOREACH( const TNS__DescribeGlobalSObjectResult &object, sobjects ) {
            kDebug() << "name=" << object.name() << "label=" << object.label()
                     << "keyPrefix=" << object.keyPrefix();
        }
#endif
    } else {
        status( Broken, message );
        error( message );
    }
}

void SalesforceResource::loginError( const KDSoapMessage &fault )
{
    mSessionId = QString();

    const QString message = fault.faultAsString();

    status( Broken, message );
    error( message );
}

#if 0
void SalesforceResource::getAvailableModulesDone( const TNS__Module_list &callResult )
{
    QString message;
    Collection::List collections;

    const TNS__Error_value errorValue = callResult.error();
    if ( !errorValue.number().isEmpty() && errorValue.number() != QLatin1String( "0" ) ) {
        kError() << "SOAP Error: number=" << errorValue.number()
                 << ", name=" << errorValue.name() << ", description=" << errorValue.description();

        message = errorValue.description();
    } else {
        Collection topLevelCollection;
        topLevelCollection.setRemoteId( identifier() );
        topLevelCollection.setName( name() );
        topLevelCollection.setParentCollection( Collection::root() );

        // Our top level collection only contains other collections (no items) and cannot be
        // modified by clients
        topLevelCollection.setContentMimeTypes( QStringList() << Collection::mimeType() );
        topLevelCollection.setRights( Collection::ReadOnly );

        collections << topLevelCollection;

        const TNS__Select_fields moduleNames = callResult.modules();
        Q_FOREACH( const QString &module, moduleNames.items() ) {
            Collection collection;

            // check if we have a corresponding module handler already
            // if not see if we can create one
            ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( module );
            if ( moduleIt != mModuleHandlers->constEnd() ) {
                collection = moduleIt.value()->collection();
            } else {
                ModuleHandler* handler = 0;
                if ( module == QLatin1String( "Contacts" ) ) {
                    handler = new ContactsHandler;
                } else {
                    //kDebug() << "No module handler for" << module;
                    continue;
                }
                mModuleHandlers->insert( module, handler );

                collection = handler->collection();
            }

            collection.setParentCollection( topLevelCollection );
            collections << collection;
        }
    }

    if ( message.isEmpty() ) {
        collectionsRetrieved( collections );
        status( Idle );
    } else {
        status( Broken, message );
        error( message );
        cancelTask( message );
    }
}

void SalesforceResource::getAvailableModulesError( const KDSoapMessage &fault )
{
    const QString message = fault.faultAsString();

    status( Broken, message );
    error( message );
    cancelTask( message );
}
#endif

void SalesforceResource::getEntryListDone( const TNS__QueryResponse &callResult )
{
    const Collection collection = currentCollection();

    Item::List items;

    // find the handler for the module represented by the given collection and let it
    // "deserialize" the SOAP response into an item payload and perform the respective "list entries" operation
    ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( collection.remoteId() );
    if ( moduleIt != mModuleHandlers->constEnd() ) {
        const TNS__QueryResult queryResult = callResult.result();
        if ( queryResult.size() > 0 ) {
            itemsRetrieved( moduleIt.value()->itemsFromListEntriesResponse( queryResult, collection ) );

            if ( !queryResult.done() ) {
                moduleIt.value()->listEntries( queryResult.queryLocator(), mSoap );
            } else {
                status( Idle );
                itemsRetrievalDone();
            }
        } else {
            status( Idle );
            itemsRetrievalDone();
        }
    }
}

void SalesforceResource::getEntryListDone( const TNS__QueryMoreResponse &callResult )
{
    const Collection collection = currentCollection();

    Item::List items;

    // find the handler for the module represented by the given collection and let it
    // "deserialize" the SOAP response into an item payload and perform the respective "list entries" operation
    ModuleHandlerHash::const_iterator moduleIt = mModuleHandlers->constFind( collection.remoteId() );
    if ( moduleIt != mModuleHandlers->constEnd() ) {
        const TNS__QueryResult queryResult = callResult.result();
        if ( queryResult.size() > 0 ) {
            itemsRetrieved( moduleIt.value()->itemsFromListEntriesResponse( queryResult, collection ) );

            if ( !queryResult.done() ) {
                moduleIt.value()->listEntries( queryResult.queryLocator(), mSoap );
            } else {
                status( Idle );
                itemsRetrievalDone();
            }
        } else {
            status( Idle );
            itemsRetrievalDone();
        }
    }
}

void SalesforceResource::getEntryListError( const KDSoapMessage &fault )
{
    const QString message = fault.faultAsString();

    status( Broken, message );
    error( message );
    cancelTask( message );
}

void SalesforceResource::setEntryDone( const TNS__UpsertResponse &callResult )
{
    QString message;

    const QList<TNS__UpsertResult> upsertResults = callResult.result();
    if ( upsertResults.isEmpty() ) {
        kError() << "UpsertResponse does not contain any results";
        message = i18nc( "@status", "Server did not respond as expected: result set is empty" );
    } else {
        if ( upsertResults.count() > 1 ) {
            kError() << "Expecting one upsert result in response but got"
                     << upsertResults.count() << ". Will just take first one";
        }

        const TNS__UpsertResult upsertResult = upsertResults[ 0 ];
        if ( !upsertResult.success() ) {
            const QList<TNS__Error> errors = upsertResult.errors();
            if ( !errors.isEmpty() ) {
                message = errors[ 0 ].message();
            } else {
                // that can probably not be reached, just to be sure
                if ( mPendingItem.remoteId().isEmpty() ) {
                    message = i18nc( "@status", "Creation of an item failed for unspecified reasons" );
                } else {
                    message = i18nc( "@status", "Modification of an item failed for unspecified reasons" );
                }
            }
        } else {
            // setting the remoteId is technically only required for the handling the result of
            // itemAdded() so Akonadi knows which identifier was assigned to the item on the server.
            mPendingItem.setRemoteId( upsertResult.id().value() );
        }
    }

    if ( !message.isEmpty() ) {
        status( Broken, message );
        error( message );
        cancelTask( message );
    } else {
        status( Idle );
        changeCommitted( mPendingItem );
    }

    mPendingItem = Item();
}

void SalesforceResource::setEntryError( const KDSoapMessage &fault )
{
    const QString message = fault.faultAsString();

    status( Broken, message );
    error( message );
    cancelTask( message );

    mPendingItem = Item();
}

void SalesforceResource::deleteEntryDone( const TNS__DeleteResponse &callResult )
{
    QString message;

    const QList<TNS__DeleteResult> deleteResults = callResult.result();
    if ( deleteResults.isEmpty() ) {
        kError() << "deleteResponse does not contain any results";
        message = i18nc( "@status", "Server did not respond as expected: result set is empty" );
    } else {
        if ( deleteResults.count() > 1 ) {
            kError() << "Expecting one delete result in response but got"
                     << deleteResults.count() << ". Will just take first one";
        }

        const TNS__DeleteResult deleteResult = deleteResults[ 0 ];
        if ( !deleteResult.success() ) {
            const QList<TNS__Error> errors = deleteResult.errors();
            if ( !errors.isEmpty() ) {
                message = errors[ 0 ].message();
            } else {
                // that can probably not be reached, just to be sure
                message = i18nc( "@status", "Deletion of an item failed for unspecified reasons" );
            }
        }
    }


    if ( !message.isEmpty() ) {
        status( Broken, message );
        error( message );
        cancelTask( message );
    } else {
        status( Idle );
        changeCommitted( mPendingItem );
    }

    mPendingItem = Item();
}

void SalesforceResource::deleteEntryError( const KDSoapMessage &fault )
{
    const QString message = fault.faultAsString();

    status( Broken, message );
    error( message );
    cancelTask( message );

    mPendingItem = Item();
}

AKONADI_RESOURCE_MAIN( SalesforceResource )

#include "salesforceresource.moc"