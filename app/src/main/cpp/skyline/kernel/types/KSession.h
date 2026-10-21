// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "KSyncObject.h"

namespace skyline::service {
    class BaseService;
}

namespace skyline::kernel::type {
    /**
     * @brief KService holds a reference to a service, this is equivalent to KClientSession
     */
    class KSession : public KSyncObject {
      public:
        std::shared_ptr<service::BaseService> serviceObject;
        // Cloned domain sessions share object IDs, but not their transport lifetime.
        struct DomainState {
            std::vector<std::shared_ptr<service::BaseService>> objects;
            KHandle nextHandle{};
        };
        std::shared_ptr<DomainState> domainState;
        bool isOpen{true}; //!< If the session is open or not
        bool isDomain{}; //!< If this is a domain session or not

        /**
         * @param serviceObject A shared pointer to the service class
         */
        KSession(const DeviceState &state, std::shared_ptr<service::BaseService> &serviceObject) : serviceObject(serviceObject), KSyncObject(state, KType::KSession) {}

        /**
         * @brief Clones the CMIF object into an independently closable session.
         * This is different from duplicating a kernel handle to this session.
         */
        std::shared_ptr<KSession> Clone() {
            auto clone{std::make_shared<KSession>(state, serviceObject)};
            clone->isDomain = isDomain;
            clone->domainState = domainState;
            return clone;
        }

        /**
         * @brief Converts this session into a domain session
         * @url https://switchbrew.org/wiki/IPC_Marshalling#Domains
         * @return The virtual handle of this service in the domain
         */
        KHandle ConvertDomain() {
            isDomain = true;
            domainState = std::make_shared<DomainState>();
            domainState->objects.push_back(serviceObject);
            return domainState->nextHandle++;
        }
    };
}
