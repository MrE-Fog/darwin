/// \file     Generator.cpp
/// \authors  jjourdin
/// \version  1.0
/// \date     07/09/18
/// \license  GPLv3
/// \brief    Copyright (c) 2018 Advens. All rights reserved.

#include <fstream>
#include <string>

#include "../../toolkit/lru_cache.hpp"
#include "../../toolkit/rapidjson/document.h"
#include "base/Logger.hpp"
#include "Generator.hpp"

Generator::Generator() {
    poolStorage = initPoolStorage();
    _configurations.flowCnf = (FlowCnf *)calloc(1, sizeof(FlowCnf));
    _configurations.streamsCnf = (StreamsCnf *)calloc(1, sizeof(StreamsCnf));
    _configurations.yaraCnf = (YaraCnf *)calloc(1, sizeof(YaraCnf));
    flowInitConfig(_configurations.flowCnf);
    yaraInitConfig(_configurations.yaraCnf);
    streamInitConfig(_configurations.streamsCnf);
    initTCPPools();

    _memoryManager = initMemoryManager();
    startMemoryManager(_memoryManager);
    DARWIN_LOGGER;
    DARWIN_LOG_DEBUG("ContentInspection:: Generator:: successfully initialised");
}

bool Generator::Configure(std::string const& configFile, const std::size_t cache_size) {
    DARWIN_LOGGER;
    DARWIN_LOG_DEBUG("ContentInspection:: Generator:: Configuring...");

    rapidjson::Document config;

    std::ifstream file(configFile.c_str());
    std::string content((std::istreambuf_iterator<char>(file)),
                        (std::istreambuf_iterator<char>()));

    DARWIN_LOG_DEBUG("ContentInspection:: Generator:: parsing file...");
    config.Parse(content.c_str());
    DARWIN_LOG_DEBUG("ContentInspection:: Generator:: file parsed");

    char str[2048];
    if(config.HasMember("maxConnections")) {
        if(config["maxConnections"].IsUint()) {
            _configurations.flowCnf->maxFlow = config["maxConnections"].GetUint();
            std::snprintf(str, 2048, "maxConnections set to %u", _configurations.flowCnf->maxFlow);
            DARWIN_LOG_DEBUG(str);
        }
        else {
            DARWIN_LOG_ERROR("'maxConnections' parameter must be a number");
        }
    }
    if(config.HasMember("yaraRuleFile")) {
        if(config["yaraRuleFile"].IsString()) {
            _configurations.yaraCnf->ruleFilename = (char *)config["yaraRuleFile"].GetString();
            FILE *yaraRuleFile = fopen(_configurations.yaraCnf->ruleFilename, "r");
            if(yaraRuleFile) {
                yaraAddRuleFile(yaraRuleFile, NULL, _configurations.yaraCnf->ruleFilename);
                fclose(yaraRuleFile);

                if(yaraCompileRules()) {
                    DARWIN_LOG_ERROR("error while compiling yara rules");
                    return false;
                }
                std::snprintf(str, 2048, "yaraRuleFile set to '%s'", config["yaraRuleFile"].GetString());
                DARWIN_LOG_DEBUG(str);
            }
        }
        else {
            DARWIN_LOG_ERROR("'yaraRuleFile' parameter must be a string");
        }
    }
    if(config.HasMember("yaraScanType")) {
        if(config["yaraScanType"].IsString()) {
            char *scanType = (char *)config["yaraScanType"].GetString();

            if(strcmp(scanType, "packet") == 0) {
                _configurations.yaraCnf->scanType = SCAN_PACKET_ONLY;
                DARWIN_LOG_INFO("set yaraScanType to 'packet'");
            }
            else if(strcmp(scanType, "stream") == 0) {
                _configurations.yaraCnf->scanType = SCAN_STREAM;
                DARWIN_LOG_INFO("set yaraScanType to 'stream'");
            }
            else {
                DARWIN_LOG_ERROR("unhandled parameter, valid YARA scan types are 'packet' and 'stream'");
                return false;
            }
        }
        else {
            DARWIN_LOG_ERROR("'yaraScanType' parameter must be a string");
        }
    }
    if(config.HasMember("yaraScanMaxSize")) {
        if(config["yaraScanMaxSize"].IsUint()) {
            _configurations.yaraCnf->scanMaxSize = config["yaraScanMaxSize"].GetUint();
            _configurations.streamsCnf->streamMaxBufferSize = config["yaraScanMaxSize"].GetUint();

            std::snprintf(str, 2048, "yaraScanMaxSize set to %u", _configurations.yaraCnf->scanMaxSize);
            DARWIN_LOG_DEBUG(str);
        }
        else {
            DARWIN_LOG_ERROR("'yaraScanMaxSize' parameter must be a number");
        }
    }
    if(config.HasMember("streamStoreFolder")) {
        if(config["streamStoreFolder"].IsString()) {
            _configurations.streamsCnf->streamStoreFolder = (char *)config["streamStoreFolder"].GetString();

            std::snprintf(str, 2048, "streamStoreFolder is '%s'", _configurations.streamsCnf->streamStoreFolder);
            DARWIN_LOG_DEBUG(str);
        }
        else {
            DARWIN_LOG_ERROR("'yaraScanMaxSize' parameter must be a string");
        }
    }
    if(config.HasMember("maxMemoryUsage")) {
        if(config["maxMemoryUsage"].IsUint()) {
            poolStorage->maxDataSize = config["maxMemoryUsage"].GetUint() * 1024 * 1024;
            std::snprintf(str, 2048, "maxMemoryUsage is %uMB", config["maxMemoryUsage"].GetUint());
            DARWIN_LOG_DEBUG(str);
        }
        else {
            DARWIN_LOG_ERROR("'maxMemoryUsage' parameter must be a number");
        }
    }

    if(_configurations.streamsCnf->streamStoreFolder) {
        if(createFolder(_configurations.streamsCnf->streamStoreFolder) != 0) {
            DARWIN_LOG_ERROR("could not create folder to dump files");
            return false;
        }
    }

    if(!_configurations.streamsCnf->streamStoreFolder && _configurations.yaraCnf->scanType == SCAN_NONE) {
        DARWIN_LOG_ERROR("missing parameters: at least 'streamStoreParameter' or 'yaraScanType' are necessary "
                         "to launch this filter");
        return false;
    }

    if(_configurations.yaraCnf->scanType && !_configurations.yaraCnf->ruleFilename) {
        DARWIN_LOG_ERROR("missing parameters: if 'yaraScanType' is given, you must provide a rule file with "
                         "the 'yaraRuleFile' parameter");
        return false;
    }

    DARWIN_LOG_DEBUG("Generator:: Cache initialization. Cache size: " + std::to_string(cache_size));
    if (cache_size > 0) {
        _cache = std::make_shared<boost::compute::detail::lru_cache<xxh::hash64_t, unsigned int>>(cache_size);
    }

    DARWIN_LOG_DEBUG("ContentInspection:: Generator:: Configured");
    return true;
}

darwin::session_ptr_t
Generator::CreateTask(boost::asio::local::stream_protocol::socket& socket,
                      darwin::Manager& manager) noexcept {
    DARWIN_LOGGER;
    return std::static_pointer_cast<darwin::Session>(
            std::make_shared<ContentInspectionTask>(socket, manager, _cache, _configurations));
}

Generator::~Generator() {
    destroyTCPPools();
    yaraDeleteConfig(_configurations.yaraCnf);
    streamDeleteConfig(_configurations.streamsCnf);
    flowDeleteConfig(_configurations.flowCnf);
    deletePoolStorage(poolStorage);
    stopMemoryManager(_memoryManager);
    DARWIN_LOGGER;
    DARWIN_LOG_INFO("ContentInspection:: Generator:: generator successfully destroyed");
}