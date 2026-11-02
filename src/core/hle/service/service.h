// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <ankerl/unordered_dense.h>
#include "common/common_types.h"
#include "core/hle/service/hle_ipc.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace Service

namespace Core {
class System;
}

namespace Kernel {
class KServerSession;
class ServiceThread;
} // namespace Kernel

namespace Service {

namespace FileSystem {
class FileSystemController;
}

namespace SM {
class ServiceManager;
}

/// Default number of maximum connections to a server session.
static constexpr u32 ServerSessionCountMax = 0x40;
static_assert(ServerSessionCountMax == 0x40,
              "ServerSessionCountMax isn't 0x40 somehow, this assert is a reminder that this will "
              "break lots of things");

/**
 * This is an non-templated base of ServiceFramework to reduce code bloat and compilation times, it
 * is not meant to be used directly.
 *
 * @see ServiceFramework
 */
class ServiceFrameworkBase : public SessionRequestHandler {
public:
    /// Returns the string identifier used to connect to the service.
    [[nodiscard]] std::string_view GetServiceName() const noexcept {
        return service_name;
    }

    /// @brief Returns the maximum number of sessions that can be connected to this service at the same
    /// time.
    u32 GetMaxSessions() const noexcept {
        return max_sessions;
    }

    /// @brief Invokes a service request routine using the HIPC protocol.
    void InvokeRequest(HLERequestContext& ctx);

    /// @brief Invokes a service request routine using the HIPC protocol.
    void InvokeRequestTipc(HLERequestContext& ctx);

    /// @brief Handles a synchronization request for the service.
    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& context) override;

protected:
    /// Member-function pointer type of SyncRequest handlers.
    template <typename Self>
    using HandlerFnP = void (Self::*)(HLERequestContext&);

    /// Used to gain exclusive access to the service members, e.g. from CoreTiming thread.
    [[nodiscard]] virtual std::unique_lock<std::mutex> LockService() noexcept {
        return std::unique_lock{lock_service};
    }
private:
    template <typename T>
    friend class ServiceFramework;

    struct FunctionInfoBase {
        u32 expected_header;
        HandlerFnP<ServiceFrameworkBase> handler_callback;
        const char* name;
    };

    using InvokerFn = void(ServiceFrameworkBase* object, HandlerFnP<ServiceFrameworkBase> member,
                           HLERequestContext& ctx);

    explicit ServiceFrameworkBase(Core::System& system_, const char* service_name_,
                                  u32 max_sessions_, InvokerFn* handler_invoker_);
    ~ServiceFrameworkBase() override;

    void RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n);
    void RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n);
    // Single-entry registration. The array forms above index through a
    // FunctionInfoBase* and so depend on the derived type having an identical
    // layout, which it does not; see RegisterHandlers below.
    void RegisterHandlerBase(const FunctionInfoBase& function);
    void RegisterHandlerBaseTipc(const FunctionInfoBase& function);
    void ReportUnimplementedFunction(HLERequestContext& ctx, const FunctionInfoBase* info);

protected:
    ankerl::unordered_dense::map<u32, FunctionInfoBase> handlers;
    ankerl::unordered_dense::map<u32, FunctionInfoBase> handlers_tipc;
    /// Used to gain exclusive access to the service members, e.g. from CoreTiming thread.
    std::mutex lock_service;
    /// System context that the service operates under.
    Core::System& system;
    /// Identifier string used to connect to the service.
    const char* service_name;
    /// Function used to safely up-cast pointers to the derived class before invoking a handler.
    InvokerFn* handler_invoker;
    /// Maximum number of concurrent sessions that this service can handle.
    u32 max_sessions;
    /// Flag to store if a port was already create/installed to detect multiple install attempts,
    /// which is not supported.
    bool service_registered = false;
};

/**
 * Framework for implementing HLE services. Dispatches on the header id of incoming SyncRequests
 * based on a table mapping header ids to handler functions. Service implementations should inherit
 * from ServiceFramework using the CRTP (`class Foo : public ServiceFramework<Foo> { ... };`) and
 * populate it with handlers by calling #RegisterHandlers.
 *
 * In order to avoid duplicating code in the binary and exposing too many implementation details in
 * the header, this class is split into a non-templated base (ServiceFrameworkBase) and a template
 * deriving from it (ServiceFramework). The functions in this class will mostly only erase the type
 * of the passed in function pointers and then delegate the actual work to the implementation in the
 * base class.
 */
template <typename Self>
class ServiceFramework : public ServiceFrameworkBase {
protected:
    /// Contains information about a request type which is handled by the service.
    template <typename T>
    struct FunctionInfoTyped : FunctionInfoBase {
        // NOT constexpr, deliberately. The original comment here said only clang
        // handles the pointer-to-member cast below without an ICE or a wrong
        // diagnostic - and it was marked constexpr regardless. On MSVC the
        // result is worse than an error: the array is constant-initialised with
        // silently wrong data. Dumping IpcController's table showed keys
        // 0,1,0,0,0,0 instead of 0..5 and garbage name pointers from entry 1
        // onward, so only 2 of its 6 handlers survived into the dispatch map.
        // QueryPointerBufferSize was one of the lost, which stalled every title
        // during CMIF session setup.
        //
        // Without constexpr the array is dynamically initialised on first use,
        // which MSVC gets right.

        /// @brief Constructs a FunctionInfo for a function.
        /// @param expected_header_ request header in the command buffer which will trigger dispatch to this handler
        /// @param handler_callback_ member function in this service which will be called to handle the request
        /// @param name_ human-friendly name for the request. Used mostly for logging purposes.
        FunctionInfoTyped(u32 expected_header_, HandlerFnP<T> handler_callback_, const char* name_)
            : FunctionInfoBase{expected_header_, HandlerFnP<ServiceFrameworkBase>(handler_callback_), name_} {}
    };
    using FunctionInfo = FunctionInfoTyped<Self>;

    /**
     * Initializes the handler with no functions installed.
     *
     * @param system_ The system context to construct this service under.
     * @param service_name_ Name of the service.
     * @param max_sessions_ Maximum number of sessions that can be connected to this service at the
     * same time.
     */
    explicit ServiceFramework(Core::System& system_, const char* service_name_, u32 max_sessions_ = ServerSessionCountMax)
        : ServiceFrameworkBase(system_, service_name_, max_sessions_, Invoker) {}

    /// Registers handlers in the service.
    template <typename T = Self, std::size_t N>
    void RegisterHandlers(const FunctionInfoTyped<T> (&functions)[N]) {
        RegisterHandlers(functions, N);
    }

    /**
     * Registers handlers in the service. Usually prefer using the other RegisterHandlers
     * overload in order to avoid needing to specify the array size.
     */
    template <typename T = Self>
    void RegisterHandlers(const FunctionInfoTyped<T>* functions, std::size_t n) {
        // Index with the *typed* pointer, one element at a time.
        //
        // This used to pass the array straight to RegisterHandlersBase, which
        // walks it as FunctionInfoBase[] - and the two layouts do not match.
        // sizeof() agrees, so a size assert passes, but the member offsets
        // differ: dumping IpcController's table showed entry 0 with its name at
        // offset 24 and entry 1 with its name at offset 20, with entry 2's key
        // landing at byte 60 rather than 64. Every element after the first was
        // read from the wrong place.
        //
        // The visible result was silent and severe: IpcController registered 2
        // of its 6 handlers, because the corrupted keys collided and emplace
        // kept only the distinct ones. QueryPointerBufferSize (command 3) was
        // among the lost, and it is part of CMIF session setup - so every title
        // stalled during early service initialisation and never reached the
        // graphics stack at all.
        for (std::size_t i = 0; i < n; ++i) {
            RegisterHandlerBase(functions[i]);
        }
    }

    /// Registers handlers in the service.
    template <typename T = Self, std::size_t N>
    void RegisterHandlersTipc(const FunctionInfoTyped<T> (&functions)[N]) {
        RegisterHandlersTipc(functions, N);
    }

    /**
     * Registers handlers in the service. Usually prefer using the other RegisterHandlers
     * overload in order to avoid needing to specify the array size.
     */
    template <typename T = Self>
    void RegisterHandlersTipc(const FunctionInfoTyped<T>* functions, std::size_t n) {
        // Same hazard as RegisterHandlers above.
        for (std::size_t i = 0; i < n; ++i) {
            RegisterHandlerBaseTipc(functions[i]);
        }
    }

protected:
    template <bool Domain, auto F>
    void CmifReplyWrap(HLERequestContext& ctx);

    /**
     * Wraps the template pointer-to-member function for use in a domain session.
     */
    template <auto F>
    static constexpr HandlerFnP<Self> D = &Self::template CmifReplyWrap<true, F>;

    /**
     * Wraps the template pointer-to-member function for use in a non-domain session.
     */
    template <auto F>
    static constexpr HandlerFnP<Self> C = &Self::template CmifReplyWrap<false, F>;

private:
    /**
     * This function is used to allow invocation of pointers to handlers stored in the base class
     * without needing to expose the type of this derived class. Pointers-to-member may require a
     * fixup when being up or downcast, and thus code that does that needs to know the concrete type
     * of the derived class in order to invoke one of it's functions through a pointer.
     */
    static void Invoker(ServiceFrameworkBase* object, HandlerFnP<ServiceFrameworkBase> member,
                        HLERequestContext& ctx) {
        // Cast back up to our original types and call the member function
        (static_cast<Self*>(object)->*HandlerFnP<Self>(member))(ctx);
    }
};

} // namespace Service
