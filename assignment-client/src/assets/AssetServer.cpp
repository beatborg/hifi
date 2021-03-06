//
//  AssetServer.cpp
//  assignment-client/src/assets
//
//  Created by Ryan Huffman on 2015/07/21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include "AssetServer.h"

#include <thread>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QString>
#include <QtGui/QImageReader>
#include <QtCore/QVector>
#include <QtCore/QUrlQuery>

#include <ClientServerUtils.h>
#include <FBXBaker.h>
#include <NodeType.h>
#include <SharedUtil.h>
#include <PathUtils.h>

#include "AssetServerLogging.h"
#include "BakeAssetTask.h"
#include "SendAssetTask.h"
#include "UploadAssetTask.h"


static const uint8_t MIN_CORES_FOR_MULTICORE = 4;
static const uint8_t CPU_AFFINITY_COUNT_HIGH = 2;
static const uint8_t CPU_AFFINITY_COUNT_LOW = 1;
#ifdef Q_OS_WIN
static const int INTERFACE_RUNNING_CHECK_FREQUENCY_MS = 1000;
#endif

const QString ASSET_SERVER_LOGGING_TARGET_NAME = "asset-server";

static const QStringList BAKEABLE_MODEL_EXTENSIONS = { "fbx" };
static QStringList BAKEABLE_TEXTURE_EXTENSIONS;
static const QString BAKED_MODEL_SIMPLE_NAME = "asset.fbx";
static const QString BAKED_TEXTURE_SIMPLE_NAME = "texture.ktx";

void AssetServer::bakeAsset(const AssetHash& assetHash, const AssetPath& assetPath, const QString& filePath) {
    qDebug() << "Starting bake for: " << assetPath << assetHash;
    auto it = _pendingBakes.find(assetHash);
    if (it == _pendingBakes.end()) {
        auto task = std::make_shared<BakeAssetTask>(assetHash, assetPath, filePath);
        task->setAutoDelete(false);
        _pendingBakes[assetHash] = task;

        connect(task.get(), &BakeAssetTask::bakeComplete, this, &AssetServer::handleCompletedBake);
        connect(task.get(), &BakeAssetTask::bakeFailed, this, &AssetServer::handleFailedBake);
        connect(task.get(), &BakeAssetTask::bakeAborted, this, &AssetServer::handleAbortedBake);

        _bakingTaskPool.start(task.get());
    } else {
        qDebug() << "Already in queue";
    }
}

QString AssetServer::getPathToAssetHash(const AssetHash& assetHash) {
    return _filesDirectory.absoluteFilePath(assetHash);
}

std::pair<BakingStatus, QString> AssetServer::getAssetStatus(const AssetPath& path, const AssetHash& hash) {
    auto it = _pendingBakes.find(hash);
    if (it != _pendingBakes.end()) {
        return { (*it)->isBaking() ? Baking : Pending, "" };
    }

    if (path.startsWith(HIDDEN_BAKED_CONTENT_FOLDER)) {
        return { Baked, "" };
    }

    auto dotIndex = path.lastIndexOf(".");
    if (dotIndex == -1) {
        return { Irrelevant, "" };
    }

    auto extension = path.mid(dotIndex + 1);

    QString bakedFilename;

    if (BAKEABLE_MODEL_EXTENSIONS.contains(extension)) {
        bakedFilename = BAKED_MODEL_SIMPLE_NAME;
    } else if (BAKEABLE_TEXTURE_EXTENSIONS.contains(extension.toLocal8Bit()) && hasMetaFile(hash)) {
        bakedFilename = BAKED_TEXTURE_SIMPLE_NAME;
    } else {
        return { Irrelevant, "" };
    }

    auto bakedPath = HIDDEN_BAKED_CONTENT_FOLDER + hash + "/" + bakedFilename;
    auto jt = _fileMappings.find(bakedPath);
    if (jt != _fileMappings.end()) {
        if (jt->second == hash) {
            return { NotBaked, "" };
        } else {
            return { Baked, "" };
        }
    } else {
        bool loaded;
        AssetMeta meta;

        std::tie(loaded, meta) = readMetaFile(hash);
        if (loaded && meta.failedLastBake) {
            return { Error, meta.lastBakeErrors };
        }
    }
    
    return { Pending, "" };
}

void AssetServer::bakeAssets() {
    auto it = _fileMappings.cbegin();
    for (; it != _fileMappings.cend(); ++it) {
        auto path = it->first;
        auto hash = it->second;
        maybeBake(path, hash);
    }
}

void AssetServer::maybeBake(const AssetPath& path, const AssetHash& hash) {
    if (needsToBeBaked(path, hash)) {
        qDebug() << "Queuing bake of: " << path;
        bakeAsset(hash, path, getPathToAssetHash(hash));
    }
}

void AssetServer::createEmptyMetaFile(const AssetHash& hash) {
    QString metaFilePath = "atp:/" + hash + "/meta.json";
    QFile metaFile { metaFilePath };
    
    if (!metaFile.exists()) {
        qDebug() << "Creating metafile for " << hash;
        if (metaFile.open(QFile::WriteOnly)) {
            qDebug() << "Created metafile for " << hash;
            metaFile.write("{}");
        }
    }
}

bool AssetServer::hasMetaFile(const AssetHash& hash) {
    QString metaFilePath = HIDDEN_BAKED_CONTENT_FOLDER + hash + "/meta.json";

    return _fileMappings.find(metaFilePath) != _fileMappings.end();
}

bool AssetServer::needsToBeBaked(const AssetPath& path, const AssetHash& assetHash) {
    if (path.startsWith(HIDDEN_BAKED_CONTENT_FOLDER)) {
        return false;
    }

    auto dotIndex = path.lastIndexOf(".");
    if (dotIndex == -1) {
        return false;
    }

    auto extension = path.mid(dotIndex + 1);

    QString bakedFilename;

    bool loaded;
    AssetMeta meta;
    std::tie(loaded, meta) = readMetaFile(assetHash);

    // TODO: Allow failed bakes that happened on old versions to be re-baked
    if (loaded && meta.failedLastBake) {
        return false;
    }

    if (BAKEABLE_MODEL_EXTENSIONS.contains(extension)) {
        bakedFilename = BAKED_MODEL_SIMPLE_NAME;
    } else if (loaded && BAKEABLE_TEXTURE_EXTENSIONS.contains(extension.toLocal8Bit())) {
        bakedFilename = BAKED_TEXTURE_SIMPLE_NAME;
    } else {
        return false;
    }

    auto bakedPath = HIDDEN_BAKED_CONTENT_FOLDER + assetHash + "/" + bakedFilename;
    return _fileMappings.find(bakedPath) == _fileMappings.end();
}

bool interfaceRunning() {
    bool result = false;

#ifdef Q_OS_WIN
    QSharedMemory sharedMemory { getInterfaceSharedMemoryName() };
    result = sharedMemory.attach(QSharedMemory::ReadOnly);
    if (result) {
        sharedMemory.detach();
    }
#endif
    return result;
}

void updateConsumedCores() {
    static bool wasInterfaceRunning = false;
    bool isInterfaceRunning = interfaceRunning();
    // If state is unchanged, return early
    if (isInterfaceRunning == wasInterfaceRunning) {
        return;
    }

    wasInterfaceRunning = isInterfaceRunning;
    auto coreCount = std::thread::hardware_concurrency();
    if (isInterfaceRunning) {
        coreCount = coreCount > MIN_CORES_FOR_MULTICORE ? CPU_AFFINITY_COUNT_HIGH : CPU_AFFINITY_COUNT_LOW;
    } 
    qCDebug(asset_server) << "Setting max consumed cores to " << coreCount;
    setMaxCores(coreCount);
}


AssetServer::AssetServer(ReceivedMessage& message) :
    ThreadedAssignment(message),
    _transferTaskPool(this),
    _bakingTaskPool(this)
{
    // store the current state of image compression so we can reset it when this assignment is complete
    _wasColorTextureCompressionEnabled = image::isColorTexturesCompressionEnabled();
    _wasGrayscaleTextureCompressionEnabled = image::isGrayscaleTexturesCompressionEnabled();
    _wasNormalTextureCompressionEnabled = image::isNormalTexturesCompressionEnabled();
    _wasCubeTextureCompressionEnabled = image::isCubeTexturesCompressionEnabled();

    // enable compression in image library
    image::setColorTexturesCompressionEnabled(true);
    image::setGrayscaleTexturesCompressionEnabled(true);
    image::setNormalTexturesCompressionEnabled(true);
    image::setCubeTexturesCompressionEnabled(true);

    BAKEABLE_TEXTURE_EXTENSIONS = TextureBaker::getSupportedFormats();
    qDebug() << "Supported baking texture formats:" << BAKEABLE_MODEL_EXTENSIONS;

    // Most of the work will be I/O bound, reading from disk and constructing packet objects,
    // so the ideal is greater than the number of cores on the system.
    static const int TASK_POOL_THREAD_COUNT = 50;
    _transferTaskPool.setMaxThreadCount(TASK_POOL_THREAD_COUNT);
    _bakingTaskPool.setMaxThreadCount(1);

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::AssetGet, this, "handleAssetGet");
    packetReceiver.registerListener(PacketType::AssetGetInfo, this, "handleAssetGetInfo");
    packetReceiver.registerListener(PacketType::AssetUpload, this, "handleAssetUpload");
    packetReceiver.registerListener(PacketType::AssetMappingOperation, this, "handleAssetMappingOperation");
    
#ifdef Q_OS_WIN
    updateConsumedCores();
    QTimer* timer = new QTimer(this);
    auto timerConnection = connect(timer, &QTimer::timeout, [] {
        updateConsumedCores();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, [this, timerConnection] {
        disconnect(timerConnection);
    });
    timer->setInterval(INTERFACE_RUNNING_CHECK_FREQUENCY_MS);
    timer->setTimerType(Qt::CoarseTimer);
    timer->start();
#endif
}

void AssetServer::aboutToFinish() {

    // remove pending transfer tasks
    _transferTaskPool.clear();

    // abort each of our still running bake tasks, remove pending bakes that were never put on the thread pool
    auto it = _pendingBakes.begin();
    while (it != _pendingBakes.end()) {
        auto pendingRunnable =  _bakingTaskPool.tryTake(it->get());

        if (pendingRunnable) {
            it = _pendingBakes.erase(it);
        } else {
            it.value()->abort();
            ++it;
        }
    }

    // make sure all bakers are finished or aborted
    while (_pendingBakes.size() > 0) {
        QCoreApplication::processEvents();
    }

    // re-set defaults in image library
    image::setColorTexturesCompressionEnabled(_wasCubeTextureCompressionEnabled);
    image::setGrayscaleTexturesCompressionEnabled(_wasGrayscaleTextureCompressionEnabled);
    image::setNormalTexturesCompressionEnabled(_wasNormalTextureCompressionEnabled);
    image::setCubeTexturesCompressionEnabled(_wasCubeTextureCompressionEnabled);
}

void AssetServer::run() {

    qCDebug(asset_server) << "Waiting for connection to domain to request settings from domain-server.";

    // wait until we have the domain-server settings, otherwise we bail
    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &AssetServer::completeSetup);
    connect(&domainHandler, &DomainHandler::settingsReceiveFail, this, &AssetServer::domainSettingsRequestFailed);

    ThreadedAssignment::commonInit(ASSET_SERVER_LOGGING_TARGET_NAME, NodeType::AssetServer);
}

static const QString ASSET_FILES_SUBDIR = "files";

void AssetServer::completeSetup() {
    auto nodeList = DependencyManager::get<NodeList>();

    auto& domainHandler = nodeList->getDomainHandler();
    const QJsonObject& settingsObject = domainHandler.getSettingsObject();

    static const QString ASSET_SERVER_SETTINGS_KEY = "asset_server";

    if (!settingsObject.contains(ASSET_SERVER_SETTINGS_KEY)) {
        qCCritical(asset_server) << "Received settings from the domain-server with no asset-server section. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetServerObject = settingsObject[ASSET_SERVER_SETTINGS_KEY].toObject();

    static const QString MAX_BANDWIDTH_OPTION = "max_bandwidth";
    auto maxBandwidthValue = assetServerObject[MAX_BANDWIDTH_OPTION];
    auto maxBandwidthFloat = maxBandwidthValue.toDouble(-1);

    if (maxBandwidthFloat > 0.0) {
        const int BITS_PER_MEGABITS = 1000 * 1000;
        int maxBandwidth = maxBandwidthFloat * BITS_PER_MEGABITS;
        nodeList->setConnectionMaxBandwidth(maxBandwidth);
        qCInfo(asset_server) << "Set maximum bandwith per connection to" << maxBandwidthFloat << "Mb/s."
                    " (" << maxBandwidth << "bits/s)";
    }

    // get the path to the asset folder from the domain server settings
    static const QString ASSETS_PATH_OPTION = "assets_path";
    auto assetsJSONValue = assetServerObject[ASSETS_PATH_OPTION];

    if (!assetsJSONValue.isString()) {
        qCCritical(asset_server) << "Received an assets path from the domain-server that could not be parsed. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetsPathString = assetsJSONValue.toString();
    QDir assetsPath { assetsPathString };
    QString absoluteFilePath = assetsPath.absolutePath();

    if (assetsPath.isRelative()) {
        // if the domain settings passed us a relative path, make an absolute path that is relative to the
        // default data directory
        absoluteFilePath = PathUtils::getAppDataFilePath("assets/" + assetsPathString);
    }

    _resourcesDirectory = QDir(absoluteFilePath);

    qCDebug(asset_server) << "Creating resources directory";
    _resourcesDirectory.mkpath(".");
    _filesDirectory = _resourcesDirectory;

    if (!_resourcesDirectory.mkpath(ASSET_FILES_SUBDIR) || !_filesDirectory.cd(ASSET_FILES_SUBDIR)) {
        qCCritical(asset_server) << "Unable to create file directory for asset-server files. Stopping assignment.";
        setFinished(true);
        return;
    }

    // load whatever mappings we currently have from the local file
    if (loadMappingsFromFile()) {
        qCInfo(asset_server) << "Serving files from: " << _filesDirectory.path();

        // Check the asset directory to output some information about what we have
        auto files = _filesDirectory.entryList(QDir::Files);

        QRegExp hashFileRegex { ASSET_HASH_REGEX_STRING };
        auto hashedFiles = files.filter(hashFileRegex);

        qCInfo(asset_server) << "There are" << hashedFiles.size() << "asset files in the asset directory.";

        if (_fileMappings.size() > 0) {
            cleanupUnmappedFiles();
        }

        nodeList->addSetOfNodeTypesToNodeInterestSet({ NodeType::Agent, NodeType::EntityScriptServer });

        bakeAssets();
    } else {
        qCCritical(asset_server) << "Asset Server assignment will not continue because mapping file could not be loaded.";
        setFinished(true);
    }
}

void AssetServer::cleanupUnmappedFiles() {
    QRegExp hashFileRegex { "^[a-f0-9]{" + QString::number(SHA256_HASH_HEX_LENGTH) + "}" };

    auto files = _filesDirectory.entryInfoList(QDir::Files);

    qCInfo(asset_server) << "Performing unmapped asset cleanup.";

    for (const auto& fileInfo : files) {
        auto filename = fileInfo.fileName();
        if (hashFileRegex.exactMatch(filename)) {
            bool matched { false };
            for (auto& pair : _fileMappings) {
                if (pair.second == filename) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                // remove the unmapped file
                QFile removeableFile { fileInfo.absoluteFilePath() };

                if (removeableFile.remove()) {
                    qCDebug(asset_server) << "\tDeleted" << filename << "from asset files directory since it is unmapped.";

                    removeBakedPathsForDeletedAsset(filename);
                } else {
                    qCDebug(asset_server) << "\tAttempt to delete unmapped file" << filename << "failed";
                }
            }
        }
    }
}

void AssetServer::handleAssetMappingOperation(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    MessageID messageID;
    message->readPrimitive(&messageID);

    AssetMappingOperationType operationType;
    message->readPrimitive(&operationType);

    auto replyPacket = NLPacketList::create(PacketType::AssetMappingOperationReply, QByteArray(), true, true);
    replyPacket->writePrimitive(messageID);

    switch (operationType) {
        case AssetMappingOperationType::Get:
            handleGetMappingOperation(*message, senderNode, *replyPacket);
            break;
        case AssetMappingOperationType::GetAll:
            handleGetAllMappingOperation(*message, senderNode, *replyPacket);
            break;
        case AssetMappingOperationType::Set:
            handleSetMappingOperation(*message, senderNode, *replyPacket);
            break;
        case AssetMappingOperationType::Delete:
            handleDeleteMappingsOperation(*message, senderNode, *replyPacket);
            break;
        case AssetMappingOperationType::Rename:
            handleRenameMappingOperation(*message, senderNode, *replyPacket);
            break;
        case AssetMappingOperationType::SetBakingEnabled:
            handleSetBakingEnabledOperation(*message, senderNode, *replyPacket);
            break;
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacketList(std::move(replyPacket), *senderNode);
}

void AssetServer::handleGetMappingOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    QString assetPath = message.readString();

    QUrl url { assetPath };
    assetPath = url.path();

    auto it = _fileMappings.find(assetPath);
    if (it != _fileMappings.end()) {

        // check if we should re-direct to a baked asset

        // first, figure out from the mapping extension what type of file this is
        auto assetPathExtension = assetPath.mid(assetPath.lastIndexOf('.') + 1).toLower();
        QString bakedRootFile;

        if (BAKEABLE_MODEL_EXTENSIONS.contains(assetPathExtension)) {
            bakedRootFile = BAKED_MODEL_SIMPLE_NAME;
        } else if (BAKEABLE_TEXTURE_EXTENSIONS.contains(assetPathExtension.toLocal8Bit())) {
            bakedRootFile = BAKED_TEXTURE_SIMPLE_NAME;
        }
        
        auto originalAssetHash = it->second;
        QString redirectedAssetHash;
        QString bakedAssetPath;
        quint8 wasRedirected = false;
        bool bakingDisabled = false;

        if (!bakedRootFile.isEmpty()) {
            // we ran into an asset for which we could have a baked version, let's check if it's ready
            bakedAssetPath = HIDDEN_BAKED_CONTENT_FOLDER + originalAssetHash + "/" + bakedRootFile;
            auto bakedIt = _fileMappings.find(bakedAssetPath);

            if (bakedIt != _fileMappings.end()) {
                if (bakedIt->second != originalAssetHash) {
                    qDebug() << "Did find baked version for: " << originalAssetHash << assetPath;
                    // we found a baked version of the requested asset to serve, redirect to that
                    redirectedAssetHash = bakedIt->second;
                    wasRedirected = true;
                } else {
                    qDebug() << "Did not find baked version for: " << originalAssetHash << assetPath << " (disabled)";
                    bakingDisabled = true;
                }
            } else {
                qDebug() << "Did not find baked version for: " << originalAssetHash << assetPath;
            }
        }

        replyPacket.writePrimitive(AssetServerError::NoError);

        if (wasRedirected) {
            qDebug() << "Writing re-directed hash for" << originalAssetHash << "to" << redirectedAssetHash;
            replyPacket.write(QByteArray::fromHex(redirectedAssetHash.toUtf8()));

            // add a flag saying that this mapping request was redirect
            replyPacket.writePrimitive(wasRedirected);

            // include the re-directed path in case the caller needs to make relative path requests for the baked asset
            replyPacket.writeString(bakedAssetPath);

        } else {
            replyPacket.write(QByteArray::fromHex(originalAssetHash.toUtf8()));
            replyPacket.writePrimitive(wasRedirected);

            auto query = QUrlQuery(url.query());
            bool isSkybox = query.hasQueryItem("skybox");
            if (isSkybox) {
                writeMetaFile(originalAssetHash);
                if (!bakingDisabled) {
                    maybeBake(assetPath, originalAssetHash);
                }
            }
        }
    } else {
        replyPacket.writePrimitive(AssetServerError::AssetNotFound);
    }
}

void AssetServer::handleGetAllMappingOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    replyPacket.writePrimitive(AssetServerError::NoError);

    uint32_t count = (uint32_t)_fileMappings.size();

    replyPacket.writePrimitive(count);

    for (auto it = _fileMappings.cbegin(); it != _fileMappings.cend(); ++ it) {
        auto mapping = it->first;
        auto hash = it->second;
        replyPacket.writeString(mapping);
        replyPacket.write(QByteArray::fromHex(hash.toUtf8()));

        BakingStatus status;
        QString lastBakeErrors;
        std::tie(status, lastBakeErrors) = getAssetStatus(mapping, hash);
        replyPacket.writePrimitive(status);
        if (status == Error) {
            replyPacket.writeString(lastBakeErrors);
        }
    }
}

void AssetServer::handleSetMappingOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    if (senderNode->getCanWriteToAssetServer()) {
        QString assetPath = message.readString();

        auto assetHash = message.read(SHA256_HASH_LENGTH).toHex();

        // don't process a set mapping operation that is inside the hidden baked folder
        if (assetPath.startsWith(HIDDEN_BAKED_CONTENT_FOLDER)) {
            qCDebug(asset_server) << "Refusing to process a set mapping operation inside" << HIDDEN_BAKED_CONTENT_FOLDER;
            replyPacket.writePrimitive(AssetServerError::PermissionDenied);
        } else {
            if (setMapping(assetPath, assetHash)) {
                replyPacket.writePrimitive(AssetServerError::NoError);
            } else {
                replyPacket.writePrimitive(AssetServerError::MappingOperationFailed);
            }
        }

    } else {
        replyPacket.writePrimitive(AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleDeleteMappingsOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    if (senderNode->getCanWriteToAssetServer()) {
        int numberOfDeletedMappings { 0 };
        message.readPrimitive(&numberOfDeletedMappings);

        QStringList mappingsToDelete;

        for (int i = 0; i < numberOfDeletedMappings; ++i) {
            auto mapping = message.readString();

            if (!mapping.startsWith(HIDDEN_BAKED_CONTENT_FOLDER)) {
                mappingsToDelete << mapping;
            } else {
                qCDebug(asset_server) << "Refusing to delete mapping" << mapping
                    << "that is inside" << HIDDEN_BAKED_CONTENT_FOLDER;
            }
        }

        if (deleteMappings(mappingsToDelete)) {
            replyPacket.writePrimitive(AssetServerError::NoError);
        } else {
            replyPacket.writePrimitive(AssetServerError::MappingOperationFailed);
        }
    } else {
        replyPacket.writePrimitive(AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleRenameMappingOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    if (senderNode->getCanWriteToAssetServer()) {
        QString oldPath = message.readString();
        QString newPath = message.readString();

        if (oldPath.startsWith(HIDDEN_BAKED_CONTENT_FOLDER) || newPath.startsWith(HIDDEN_BAKED_CONTENT_FOLDER)) {
            qCDebug(asset_server) << "Cannot rename" << oldPath << "to" << newPath
                << "since one of the paths is inside" << HIDDEN_BAKED_CONTENT_FOLDER;
            replyPacket.writePrimitive(AssetServerError::PermissionDenied);
        } else {
            if (renameMapping(oldPath, newPath)) {
                replyPacket.writePrimitive(AssetServerError::NoError);
            } else {
                replyPacket.writePrimitive(AssetServerError::MappingOperationFailed);
            }
        }

    } else {
        replyPacket.writePrimitive(AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleSetBakingEnabledOperation(ReceivedMessage& message, SharedNodePointer senderNode, NLPacketList& replyPacket) {
    if (senderNode->getCanWriteToAssetServer()) {
        bool enabled { true };
        message.readPrimitive(&enabled);

        int numberOfMappings{ 0 };
        message.readPrimitive(&numberOfMappings);

        QStringList mappings;

        for (int i = 0; i < numberOfMappings; ++i) {
            mappings << message.readString();
        }

        if (setBakingEnabled(mappings, enabled)) {
            replyPacket.writePrimitive(AssetServerError::NoError);
        } else {
            replyPacket.writePrimitive(AssetServerError::MappingOperationFailed);
        }
    } else {
        replyPacket.writePrimitive(AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleAssetGetInfo(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    QByteArray assetHash;
    MessageID messageID;

    if (message->getSize() < qint64(SHA256_HASH_LENGTH + sizeof(messageID))) {
        qCDebug(asset_server) << "ERROR bad file request";
        return;
    }

    message->readPrimitive(&messageID);
    assetHash = message->readWithoutCopy(SHA256_HASH_LENGTH);

    auto size = qint64(sizeof(MessageID) + SHA256_HASH_LENGTH + sizeof(AssetServerError) + sizeof(qint64));
    auto replyPacket = NLPacket::create(PacketType::AssetGetInfoReply, size, true);

    QByteArray hexHash = assetHash.toHex();

    replyPacket->writePrimitive(messageID);
    replyPacket->write(assetHash);

    QString fileName = QString(hexHash);
    QFileInfo fileInfo { _filesDirectory.filePath(fileName) };

    if (fileInfo.exists() && fileInfo.isReadable()) {
        qCDebug(asset_server) << "Opening file: " << fileInfo.filePath();
        replyPacket->writePrimitive(AssetServerError::NoError);
        replyPacket->writePrimitive(fileInfo.size());
    } else {
        qCDebug(asset_server) << "Asset not found: " << QString(hexHash);
        replyPacket->writePrimitive(AssetServerError::AssetNotFound);
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

void AssetServer::handleAssetGet(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {

    auto minSize = qint64(sizeof(MessageID) + SHA256_HASH_LENGTH + sizeof(DataOffset) + sizeof(DataOffset));

    if (message->getSize() < minSize) {
        qCDebug(asset_server) << "ERROR bad file request";
        return;
    }

    // Queue task
    auto task = new SendAssetTask(message, senderNode, _filesDirectory);
    _transferTaskPool.start(task);
}

void AssetServer::handleAssetUpload(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {

    if (senderNode->getCanWriteToAssetServer()) {
        qCDebug(asset_server) << "Starting an UploadAssetTask for upload from" << uuidStringWithoutCurlyBraces(senderNode->getUUID());

        auto task = new UploadAssetTask(message, senderNode, _filesDirectory);
        _transferTaskPool.start(task);
    } else {
        // this is a node the domain told us is not allowed to rez entities
        // for now this also means it isn't allowed to add assets
        // so return a packet with error that indicates that

        auto permissionErrorPacket = NLPacket::create(PacketType::AssetUploadReply, sizeof(MessageID) + sizeof(AssetServerError), true);

        MessageID messageID;
        message->readPrimitive(&messageID);

        // write the message ID and a permission denied error
        permissionErrorPacket->writePrimitive(messageID);
        permissionErrorPacket->writePrimitive(AssetServerError::PermissionDenied);

        // send off the packet
        auto nodeList = DependencyManager::get<NodeList>();
        nodeList->sendPacket(std::move(permissionErrorPacket), *senderNode);
    }
}

void AssetServer::sendStatsPacket() {
    QJsonObject serverStats;

    auto stats = DependencyManager::get<NodeList>()->sampleStatsForAllConnections();

    for (const auto& stat : stats) {
        QJsonObject nodeStats;
        auto endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(stat.second.endTime);
        QDateTime date = QDateTime::fromMSecsSinceEpoch(endTimeMs.count());

        static const float USEC_PER_SEC = 1000000.0f;
        static const float MEGABITS_PER_BYTE = 8.0f / 1000000.0f; // Bytes => Mbits
        float elapsed = (float)(stat.second.endTime - stat.second.startTime).count() / USEC_PER_SEC; // sec
        float megabitsPerSecPerByte = MEGABITS_PER_BYTE / elapsed; // Bytes => Mb/s

        QJsonObject connectionStats;
        connectionStats["1. Last Heard"] = date.toString();
        connectionStats["2. Est. Max (P/s)"] = stat.second.estimatedBandwith;
        connectionStats["3. RTT (ms)"] = stat.second.rtt;
        connectionStats["4. CW (P)"] = stat.second.congestionWindowSize;
        connectionStats["5. Period (us)"] = stat.second.packetSendPeriod;
        connectionStats["6. Up (Mb/s)"] = stat.second.sentBytes * megabitsPerSecPerByte;
        connectionStats["7. Down (Mb/s)"] = stat.second.receivedBytes * megabitsPerSecPerByte;
        nodeStats["Connection Stats"] = connectionStats;

        using Events = udt::ConnectionStats::Stats::Event;
        const auto& events = stat.second.events;

        QJsonObject upstreamStats;
        upstreamStats["1. Sent (P/s)"] = stat.second.sendRate;
        upstreamStats["2. Sent Packets"] = stat.second.sentPackets;
        upstreamStats["3. Recvd ACK"] = events[Events::ReceivedACK];
        upstreamStats["4. Procd ACK"] = events[Events::ProcessedACK];
        upstreamStats["5. Recvd LACK"] = events[Events::ReceivedLightACK];
        upstreamStats["6. Recvd NAK"] = events[Events::ReceivedNAK];
        upstreamStats["7. Recvd TNAK"] = events[Events::ReceivedTimeoutNAK];
        upstreamStats["8. Sent ACK2"] = events[Events::SentACK2];
        upstreamStats["9. Retransmitted"] = events[Events::Retransmission];
        nodeStats["Upstream Stats"] = upstreamStats;

        QJsonObject downstreamStats;
        downstreamStats["1. Recvd (P/s)"] = stat.second.receiveRate;
        downstreamStats["2. Recvd Packets"] = stat.second.receivedPackets;
        downstreamStats["3. Sent ACK"] = events[Events::SentACK];
        downstreamStats["4. Sent LACK"] = events[Events::SentLightACK];
        downstreamStats["5. Sent NAK"] = events[Events::SentNAK];
        downstreamStats["6. Sent TNAK"] = events[Events::SentTimeoutNAK];
        downstreamStats["7. Recvd ACK2"] = events[Events::ReceivedACK2];
        downstreamStats["8. Duplicates"] = events[Events::Duplicate];
        nodeStats["Downstream Stats"] = downstreamStats;

        QString uuid;
        auto nodelist = DependencyManager::get<NodeList>();
        if (stat.first == nodelist->getDomainHandler().getSockAddr()) {
            uuid = uuidStringWithoutCurlyBraces(nodelist->getDomainHandler().getUUID());
            nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = "DomainServer";
        } else {
            auto node = nodelist->findNodeWithAddr(stat.first);
            uuid = uuidStringWithoutCurlyBraces(node ? node->getUUID() : QUuid());
            nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = uuid;
        }

        serverStats[uuid] = nodeStats;
    }

    // send off the stats packets
    ThreadedAssignment::addPacketStatsAndSendStatsPacket(serverStats);
}

static const QString MAP_FILE_NAME = "map.json";

bool AssetServer::loadMappingsFromFile() {

    auto mapFilePath = _resourcesDirectory.absoluteFilePath(MAP_FILE_NAME);

    QFile mapFile { mapFilePath };
    if (mapFile.exists()) {
        if (mapFile.open(QIODevice::ReadOnly)) {
            QJsonParseError error;

            auto jsonDocument = QJsonDocument::fromJson(mapFile.readAll(), &error);

            if (error.error == QJsonParseError::NoError) {
                if (!jsonDocument.isObject()) {
                    qCWarning(asset_server) << "Failed to read mapping file, root value in" << mapFilePath << "is not an object";
                    return false;
                }

                //_fileMappings = jsonDocument.object().toVariantHash();
                auto root = jsonDocument.object();
                for (auto it = root.begin(); it != root.end(); ++it) {
                    auto key = it.key();
                    auto value = it.value();

                    if (!value.isString()) {
                        qCWarning(asset_server) << "Skipping" << key << ":" << value << "because it is not a string";
                        continue;
                    }

                    if (!isValidFilePath(key)) {
                        qCWarning(asset_server) << "Will not keep mapping for" << key << "since it is not a valid path.";
                        continue;
                    }

                    if (!isValidHash(value.toString())) {
                        qCWarning(asset_server) << "Will not keep mapping for" << key << "since it does not have a valid hash.";
                        continue;
                    }


                    qDebug() << "Added " << key << value.toString();
                    _fileMappings[key] = value.toString();
                }

                qCInfo(asset_server) << "Loaded" << _fileMappings.size() << "mappings from map file at" << mapFilePath;
                return true;
            }
        }

        qCCritical(asset_server) << "Failed to read mapping file at" << mapFilePath;
        return false;
    } else {
        qCInfo(asset_server) << "No existing mappings loaded from file since no file was found at" << mapFilePath;
    }

    return true;
}

bool AssetServer::writeMappingsToFile() {
    auto mapFilePath = _resourcesDirectory.absoluteFilePath(MAP_FILE_NAME);

    QFile mapFile { mapFilePath };
    if (mapFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;

        for (auto it : _fileMappings) {
            root[it.first] = it.second;
        }

        QJsonDocument jsonDocument { root };

        if (mapFile.write(jsonDocument.toJson()) != -1) {
            qCDebug(asset_server) << "Wrote JSON mappings to file at" << mapFilePath;
            return true;
        } else {
            qCWarning(asset_server) << "Failed to write JSON mappings to file at" << mapFilePath;
        }
    } else {
        qCWarning(asset_server) << "Failed to open map file at" << mapFilePath;
    }

    return false;
}

bool AssetServer::setMapping(AssetPath path, AssetHash hash) {
    path = path.trimmed();

    if (!isValidFilePath(path)) {
        qCWarning(asset_server) << "Cannot set a mapping for invalid path:" << path << "=>" << hash;
        return false;
    }

    if (!isValidHash(hash)) {
        qCWarning(asset_server) << "Cannot set a mapping for invalid hash" << path << "=>" << hash;
        return false;
    }

    // remember what the old mapping was in case persistence fails
    auto it = _fileMappings.find(path);
    auto oldMapping = it != _fileMappings.end() ? it->second : "";

    // update the in memory QHash
    _fileMappings[path] = hash;

    // attempt to write to file
    if (writeMappingsToFile()) {
        // persistence succeeded, we are good to go
        qCDebug(asset_server) << "Set mapping:" << path << "=>" << hash;
        maybeBake(path, hash);
        return true;
    } else {
        // failed to persist this mapping to file - put back the old one in our in-memory representation
        if (oldMapping.isEmpty()) {
            _fileMappings.erase(_fileMappings.find(path));
        } else {
            _fileMappings[path] = oldMapping;
        }

        qCWarning(asset_server) << "Failed to persist mapping:" << path << "=>" << hash;

        return false;
    }
}

bool pathIsFolder(const AssetPath& path) {
    return path.endsWith('/');
}

void AssetServer::removeBakedPathsForDeletedAsset(AssetHash hash) {
    // we deleted the file with this hash

    // check if we had baked content for that file that should also now be removed
    // by calling deleteMappings for the hidden baked content folder for this hash
    AssetPathList hiddenBakedFolder { HIDDEN_BAKED_CONTENT_FOLDER + hash + "/" };

    qCDebug(asset_server) << "Deleting baked content below" << hiddenBakedFolder << "since" << hash << "was deleted";

    deleteMappings(hiddenBakedFolder);
}

bool AssetServer::deleteMappings(const AssetPathList& paths) {
    // take a copy of the current mappings in case persistence of these deletes fails
    auto oldMappings = _fileMappings;

    QSet<QString> hashesToCheckForDeletion;

    // enumerate the paths to delete and remove them all
    for (const auto& rawPath : paths) {
        auto path = rawPath.trimmed();

        // figure out if this path will delete a file or folder
        if (pathIsFolder(path)) {
            // enumerate the in memory file mappings and remove anything that matches
            auto it = _fileMappings.begin();
            auto sizeBefore = _fileMappings.size();

            while (it != _fileMappings.end()) {
                if (it->first.startsWith(path)) {
                    // add this hash to the list we need to check for asset removal from the server
                    hashesToCheckForDeletion << it->second;

                    it = _fileMappings.erase(it);
                } else {
                    ++it;
                }
            }

            auto sizeNow = _fileMappings.size();
            if (sizeBefore != sizeNow) {
                qCDebug(asset_server) << "Deleted" << sizeBefore - sizeNow << "mappings in folder: " << path;
            } else {
                qCDebug(asset_server) << "Did not find any mappings to delete in folder:" << path;
            }

        } else {
            auto it = _fileMappings.find(path);
            if (it != _fileMappings.end()) {
                // add this hash to the list we need to check for asset removal from server
                hashesToCheckForDeletion << it->second;

                qCDebug(asset_server) << "Deleted a mapping:" << path << "=>" << it->second;
                
                _fileMappings.erase(it);
            } else {
                qCDebug(asset_server) << "Unable to delete a mapping that was not found:" << path;
            }
        }
    }

    // deleted the old mappings, attempt to persist to file
    if (writeMappingsToFile()) {
        // persistence succeeded we are good to go

        // TODO iterate through hashesToCheckForDeletion instead
        for (auto& pair : _fileMappings) {
            auto it = hashesToCheckForDeletion.find(pair.second);
            if (it != hashesToCheckForDeletion.end()) {
                hashesToCheckForDeletion.erase(it);
            }
        }

        // we now have a set of hashes that are unmapped - we will delete those asset files
        for (auto& hash : hashesToCheckForDeletion) {
            // remove the unmapped file
            QFile removeableFile { _filesDirectory.absoluteFilePath(hash) };

            if (removeableFile.remove()) {
                qCDebug(asset_server) << "\tDeleted" << hash << "from asset files directory since it is now unmapped.";

                removeBakedPathsForDeletedAsset(hash);
            } else {
                qCDebug(asset_server) << "\tAttempt to delete unmapped file" << hash << "failed";
            }
        }

        return true;
    } else {
        qCWarning(asset_server) << "Failed to persist deleted mappings, rolling back";

        // we didn't delete the previous mapping, put it back in our in-memory representation
        _fileMappings = oldMappings;

        return false;
    }
}

bool AssetServer::renameMapping(AssetPath oldPath, AssetPath newPath) {
    oldPath = oldPath.trimmed();
    newPath = newPath.trimmed();

    if (!isValidFilePath(oldPath) || !isValidFilePath(newPath)) {
        qCWarning(asset_server) << "Cannot perform rename with invalid paths - both should have leading forward and no ending slashes:"
            << oldPath << "=>" << newPath;

        return false;
    }

    // figure out if this rename is for a file or folder
    if (pathIsFolder(oldPath)) {
        if (!pathIsFolder(newPath)) {
            // we were asked to rename a path to a folder to a path that isn't a folder, this is a fail
            qCWarning(asset_server) << "Cannot rename mapping from folder path" << oldPath << "to file path" << newPath;

            return false;
        }

        // take a copy of the old mappings
        auto oldMappings = _fileMappings;

        // iterate the current mappings and adjust any that matches the renamed folder
        auto it = oldMappings.begin();

        while (it != oldMappings.end()) {
            auto& oldKey = it->first;
            if (oldKey.startsWith(oldPath)) {
                auto newKey = oldKey;
                newKey.replace(0, oldPath.size(), newPath);

                // remove the old version from the in memory file mappings
                _fileMappings.erase(_fileMappings.find(oldKey));
                _fileMappings[newKey] = it->second;
            }

            ++it;
        }

        if (writeMappingsToFile()) {
            // persisted the changed mappings, return success
            qCDebug(asset_server) << "Renamed folder mapping:" << oldPath << "=>" << newPath;

            return true;
        } else {
            // couldn't persist the renamed paths, rollback and return failure
            _fileMappings = oldMappings;

            qCWarning(asset_server) << "Failed to persist renamed folder mapping:" << oldPath << "=>" << newPath;

            return false;
        }
    } else {
        if (pathIsFolder(newPath)) {
            // we were asked to rename a path to a file to a path that is a folder, this is a fail
            qCWarning(asset_server) << "Cannot rename mapping from file path" << oldPath << "to folder path" << newPath;

            return false;
        }

        // take the old hash to remove the old mapping
        auto it = _fileMappings.find(oldPath);
        auto oldSourceMapping = it->second;
        _fileMappings.erase(it);

        // in case we're overwriting, keep the current destination mapping for potential rollback
        auto oldDestinationIt = _fileMappings.find(newPath);

        if (!oldSourceMapping.isEmpty()) {
            _fileMappings[newPath] = oldSourceMapping;

            if (writeMappingsToFile()) {
                // persisted the renamed mapping, return success
                qCDebug(asset_server) << "Renamed mapping:" << oldPath << "=>" << newPath;

                return true;
            } else {
                // we couldn't persist the renamed mapping, rollback and return failure
                _fileMappings[oldPath] = oldSourceMapping;

                if (oldDestinationIt != _fileMappings.end()) {
                    // put back the overwritten mapping for the destination path
                    _fileMappings[newPath] = oldDestinationIt->second;
                } else {
                    // clear the new mapping
                    _fileMappings.erase(_fileMappings.find(newPath));
                }

                qCDebug(asset_server) << "Failed to persist renamed mapping:" << oldPath << "=>" << newPath;

                return false;
            }
        } else {
            // failed to find a mapping that was to be renamed, return failure
            return false;
        }
    }
}

static const QString BAKED_ASSET_SIMPLE_FBX_NAME = "asset.fbx";
static const QString BAKED_ASSET_SIMPLE_TEXTURE_NAME = "texture.ktx";

QString getBakeMapping(const AssetHash& hash, const QString& relativeFilePath) {
    return HIDDEN_BAKED_CONTENT_FOLDER + hash + "/" + relativeFilePath;
}

void AssetServer::handleFailedBake(QString originalAssetHash, QString assetPath, QString errors) {
    qDebug() << "Failed: " << originalAssetHash << assetPath << errors;

    bool loaded;
    AssetMeta meta;

    std::tie(loaded, meta) = readMetaFile(originalAssetHash);

    meta.failedLastBake = true;
    meta.lastBakeErrors = errors;

    writeMetaFile(originalAssetHash, meta);

    _pendingBakes.remove(originalAssetHash);
}

void AssetServer::handleCompletedBake(QString originalAssetHash, QString originalAssetPath,
                                      QString bakedTempOutputDir, QVector<QString> bakedFilePaths) {
    bool errorCompletingBake { false };
    QString errorReason;

    qDebug() << "Completing bake for " << originalAssetHash;

    for (auto& filePath : bakedFilePaths) {
        // figure out the hash for the contents of this file
        QFile file(filePath);

        qDebug() << "File path: " << filePath;

        AssetHash bakedFileHash;

        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hasher(QCryptographicHash::Sha256);

            if (hasher.addData(&file)) {
                bakedFileHash = hasher.result().toHex();
            } else {
                // stop handling this bake, couldn't hash the contents of the file
                errorCompletingBake = true;
                errorReason = "Failed to finalize bake";
                break;
            }

            // first check that we don't already have this bake file in our list
            auto bakeFileDestination = _filesDirectory.absoluteFilePath(bakedFileHash);
            if (!QFile::exists(bakeFileDestination)) {
                // copy each to our files folder (with the hash as their filename)
                if (!file.copy(_filesDirectory.absoluteFilePath(bakedFileHash))) {
                    // stop handling this bake, couldn't copy the bake file into our files directory
                    errorCompletingBake = true;
                    errorReason = "Failed to copy baked assets to asset server";
                    break;
                }
            }

            // setup the mapping for this bake file
            auto relativeFilePath = QUrl(filePath).fileName();
            qDebug() << "Relative file path is: " << relativeFilePath;

            if (relativeFilePath.endsWith(".fbx", Qt::CaseInsensitive)) {
                // for an FBX file, we replace the filename with the simple name
                // (to handle the case where two mapped assets have the same hash but different names)
                relativeFilePath = BAKED_ASSET_SIMPLE_FBX_NAME;
            } else if (!originalAssetPath.endsWith(".fbx", Qt::CaseInsensitive)) {
                relativeFilePath = BAKED_ASSET_SIMPLE_TEXTURE_NAME;

            }

            QString bakeMapping = getBakeMapping(originalAssetHash, relativeFilePath);

            // add a mapping (under the hidden baked folder) for this file resulting from the bake
            if (setMapping(bakeMapping, bakedFileHash)) {
                qDebug() << "Added" << bakeMapping << "for bake file" << bakedFileHash << "from bake of" << originalAssetHash;
            } else {
                qDebug() << "Failed to set mapping";
                // stop handling this bake, couldn't add a mapping for this bake file
                errorCompletingBake = true;
                errorReason = "Failed to finalize bake";
                break;
            }
        } else {
            qDebug() << "Failed to open baked file: " << filePath;
            // stop handling this bake, we couldn't open one of the files for reading
            errorCompletingBake = true;
            errorReason = "Failed to finalize bake";
            break;
        }
    }

    for (auto& filePath : bakedFilePaths) {
        QFile file(filePath);
        if (!file.remove()) {
            qWarning() << "Failed to remove temporary file:" << filePath;
        }
    }
    if (!QDir(bakedTempOutputDir).rmdir(".")) {
        qWarning() << "Failed to remove temporary directory:" << bakedTempOutputDir;
    }

    if (!errorCompletingBake) {
        // create the meta file to store which version of the baking process we just completed
        writeMetaFile(originalAssetHash);
    } else {
        qWarning() << "Could not complete bake for" << originalAssetHash;
        AssetMeta meta;
        meta.failedLastBake = true;
        meta.lastBakeErrors = errorReason;
        writeMetaFile(originalAssetHash, meta);
    }

    _pendingBakes.remove(originalAssetHash);
}

void AssetServer::handleAbortedBake(QString originalAssetHash, QString assetPath) {
    // for an aborted bake we don't do anything but remove the BakeAssetTask from our pending bakes
    _pendingBakes.remove(originalAssetHash);
}

static const QString BAKE_VERSION_KEY = "bake_version";
static const QString FAILED_LAST_BAKE_KEY = "failed_last_bake";
static const QString LAST_BAKE_ERRORS_KEY = "last_bake_errors";

std::pair<bool, AssetMeta> AssetServer::readMetaFile(AssetHash hash) {
    auto metaFilePath = HIDDEN_BAKED_CONTENT_FOLDER + hash + "/" + "meta.json";

    auto it = _fileMappings.find(metaFilePath);
    if (it == _fileMappings.end()) {
        return { false, {} };
    }

    auto metaFileHash = it->second;

    QFile metaFile(_filesDirectory.absoluteFilePath(metaFileHash));

    if (metaFile.open(QIODevice::ReadOnly)) {
        auto data = metaFile.readAll();
        metaFile.close();

        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(data, &error);

        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            auto root = doc.object();

            auto bakeVersion = root[BAKE_VERSION_KEY].toInt(-1);
            auto failedLastBake = root[FAILED_LAST_BAKE_KEY];
            auto lastBakeErrors = root[LAST_BAKE_ERRORS_KEY];

            if (bakeVersion != -1
                && failedLastBake.isBool()
                && lastBakeErrors.isString()) {

                AssetMeta meta;
                meta.bakeVersion = bakeVersion;
                meta.failedLastBake = failedLastBake.toBool();
                meta.lastBakeErrors = lastBakeErrors.toString();

                return { true, meta };
            } else {
                qCWarning(asset_server) << "Metafile for" << hash << "has either missing or malformed data.";
            }
        }
    }

    return { false, {} };
}

bool AssetServer::writeMetaFile(AssetHash originalAssetHash, const AssetMeta& meta) {
    // construct the JSON that will be in the meta file
    QJsonObject metaFileObject;

    metaFileObject[BAKE_VERSION_KEY] = meta.bakeVersion;
    metaFileObject[FAILED_LAST_BAKE_KEY] = meta.failedLastBake;
    metaFileObject[LAST_BAKE_ERRORS_KEY] = meta.lastBakeErrors;

    QJsonDocument metaFileDoc;
    metaFileDoc.setObject(metaFileObject);

    auto metaFileJSON = metaFileDoc.toJson();

    // get a hash for the contents of the meta-file
    AssetHash metaFileHash = QCryptographicHash::hash(metaFileJSON, QCryptographicHash::Sha256).toHex();

    // create the meta file in our files folder, named by the hash of its contents
    QFile metaFile(_filesDirectory.absoluteFilePath(metaFileHash));

    if (metaFile.open(QIODevice::WriteOnly)) {
        metaFile.write(metaFileJSON);
        metaFile.close();

        // add a mapping to the meta file so it doesn't get deleted because it is unmapped
        auto metaFileMapping = HIDDEN_BAKED_CONTENT_FOLDER + originalAssetHash + "/" + "meta.json";

        return setMapping(metaFileMapping, metaFileHash);
    } else {
        return false;
    }
}

bool AssetServer::setBakingEnabled(const AssetPathList& paths, bool enabled) {
    for (const auto& path : paths) {
        auto it = _fileMappings.find(path);
        if (it != _fileMappings.end()) {
            auto hash = it->second;

            auto dotIndex = path.lastIndexOf(".");
            if (dotIndex == -1) {
                continue;
            }

            auto extension = path.mid(dotIndex + 1);

            QString bakedFilename;
    
            if (BAKEABLE_MODEL_EXTENSIONS.contains(extension)) {
                bakedFilename = BAKED_MODEL_SIMPLE_NAME;
            } else if (BAKEABLE_TEXTURE_EXTENSIONS.contains(extension.toLocal8Bit()) && hasMetaFile(hash)) {
                bakedFilename = BAKED_TEXTURE_SIMPLE_NAME;
            } else {
                continue;
            }

            auto bakedMapping = getBakeMapping(hash, bakedFilename);

            auto it = _fileMappings.find(bakedMapping);
            bool currentlyDisabled = (it != _fileMappings.end() && it->second == hash);

            if (enabled && currentlyDisabled) {
                QStringList bakedMappings{ bakedMapping };
                deleteMappings(bakedMappings);
                maybeBake(path, hash);
                qDebug() << "Enabled baking for" << path;
            } else if (!enabled && !currentlyDisabled) {
                removeBakedPathsForDeletedAsset(hash);
                setMapping(bakedMapping, hash);
                qDebug() << "Disabled baking for" << path;
            }
        }
    }
    return true;
}
