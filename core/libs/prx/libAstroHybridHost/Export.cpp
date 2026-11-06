#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostHdr.hpp"

namespace {

constexpr std::uint32_t KytyGpuAbiVersion = (1u << 16u) | 2u;

struct PadTouch {
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t active;
    std::uint8_t reserved[2];
};

struct PadState {
    std::uint32_t buttons;
    std::uint8_t sticks[4];
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t connected;
    std::uint8_t reserved;
    PadTouch touch[2];
};

static_assert(sizeof(PadState) == 28);
constexpr std::uint32_t EventEopInterrupt = 1;
constexpr std::uint32_t EventFlip = 2;
constexpr std::int16_t FilterGraphicsCore = -14;
constexpr std::int16_t FilterVideoOut = -13;
constexpr std::uintptr_t VideoOutEventFlip = 0;
constexpr int ErrorBadQueue = static_cast<int>(0x80020009u);
constexpr std::size_t DonationAlignment = std::size_t{1} << 30u;

struct HostCallbacks {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    void* user;
    void (*log)(void* user, int level, const char* message);
    int (*addGpuEvent)(void* user, std::int64_t equeue, std::uint32_t kind, std::int64_t ident, void* udata);
    int (*deleteGpuEvent)(void* user, std::int64_t equeue, std::uint32_t kind, std::int64_t ident);
    void (*deliverEvent)(void* user, std::uint32_t kind, std::int64_t ident, std::uint64_t data);
    int (*queryMemory)(void* user, std::uint64_t address, std::uint64_t* base, std::uint64_t* size, std::uint32_t* flags, const void** alias);
    const char* (*app0Path)(void* user);
    int (*donateGuestRange)(void* user, std::uint64_t size, std::uint64_t* base);
    void (*publishPad)(void* user, int port, const PadState* state);
};

static_assert(sizeof(HostCallbacks) == 80);

struct Registration {
    KernelEqueue eq;
    std::uint32_t kind;
    std::int64_t ident;
};

std::mutex g_mutex;
std::vector<Registration> g_registrations;

void Log(void*, int level, const char* message) {
    std::fprintf(stderr, "[astrohybrid %d] %s\n", level, message);
}

std::int16_t FilterOf(std::uint32_t kind) {
    return kind == EventFlip ? FilterVideoOut : FilterGraphicsCore;
}

std::uintptr_t IdentOf(std::uint32_t kind, std::int64_t ident) {
    return kind == EventFlip ? VideoOutEventFlip : static_cast<std::uintptr_t>(ident);
}

int AddGpuEvent(void*, std::int64_t equeue, std::uint32_t kind, std::int64_t ident, void* udata) {
    if (equeue == 0 || !EqueuePin_nid_postfix(equeue)) return ErrorBadQueue;
    if (kind != EventEopInterrupt && kind != EventFlip) return ErrorBadQueue;
    KernelEqueueEvent event{};
    event.event.ident = IdentOf(kind, ident);
    event.event.filter = FilterOf(kind);
    event.event.flags = kind == EventFlip ? EV_ADD : EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    if (kind == EventFlip) {
        event.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
            KernelEvent triggered = e->event;
            triggered.fflags = triggered.fflags < 0xfu ? triggered.fflags + 1u : triggered.fflags;
            triggered.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(data));
            if (e->triggered) {
                e->pendingEvents.push_back(triggered);
            } else {
                e->event = triggered;
                e->triggered = true;
            }
        };
        event.filter.resetFunc = [](KernelEqueueEvent* e) {
            e->triggered = false;
            e->event.fflags = 0;
            e->event.data = 0;
        };
    } else {
        event.filter.triggerFunc = [](KernelEqueueEvent* e, void*) {
            e->event.data = e->triggered ? e->event.data + 1 : 1;
            e->triggered = true;
        };
        event.filter.resetFunc = [](KernelEqueueEvent* e) {
            e->triggered = false;
            e->event.data = 0;
        };
    }
    const int result = EqueueAddEvent_nid_postfix(equeue, event);
    if (result != EQUEUE_OK) return result;
    std::lock_guard lock(g_mutex);
    if (std::none_of(g_registrations.begin(), g_registrations.end(), [&](const Registration& r) { return r.eq == equeue && r.kind == kind && r.ident == ident; })) {
        g_registrations.push_back({equeue, kind, ident});
    }
    return 0;
}

int DeleteGpuEvent(void*, std::int64_t equeue, std::uint32_t kind, std::int64_t ident) {
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_registrations, [&](const Registration& r) { return r.eq == equeue && r.kind == kind && r.ident == ident; });
    }
    const int result = EqueueDeleteEvent_nid_postfix(equeue, IdentOf(kind, ident), FilterOf(kind));
    return result == EQUEUE_OK ? 0 : result;
}

void DeliverEvent(void*, std::uint32_t kind, std::int64_t ident, std::uint64_t data) {
    std::vector<Registration> registrations;
    {
        std::lock_guard lock(g_mutex);
        registrations = g_registrations;
    }
    for (const auto& registration : registrations) {
        if (registration.kind != kind || (kind == EventEopInterrupt && registration.ident != ident)) continue;
        EqueueTriggerEvent_nid_postfix(registration.eq, IdentOf(kind, registration.ident), FilterOf(kind), reinterpret_cast<void*>(static_cast<uintptr_t>(data)));
    }
}

int DonateGuestRange(void*, std::uint64_t size, std::uint64_t* base) {
    if (base == nullptr || size == 0) return -1;
    const auto donated = *base != 0 ? GuestArena::GuestArenaDonateAt_nid_postfix(static_cast<std::uintptr_t>(*base), static_cast<std::size_t>(size)) : GuestArena::GuestArenaDonate_nid_postfix(static_cast<std::size_t>(size), DonationAlignment);
    if (donated == 0) return -1;
    *base = donated;
    return 0;
}

void PublishPad(void*, int port, const PadState* state) {
    if (state == nullptr) return;
    PadInputState input{};
    input.buttons = state->buttons;
    for (std::size_t i = 0; i < input.sticks.size(); ++i) input.sticks[i] = state->sticks[i];
    input.analogButtonsL2 = state->l2;
    input.analogButtonsR2 = state->r2;
    for (std::size_t i = 0; i < input.touch.size(); ++i) {
        input.touch[i].active = state->touch[i].active != 0;
        input.touch[i].x = state->touch[i].x;
        input.touch[i].y = state->touch[i].y;
        input.touch[i].id = state->touch[i].id;
    }
    PadPublishInputForPort_nid_postfix(port, input);
}

const HostCallbacks g_callbacks{sizeof(HostCallbacks), KytyGpuAbiVersion, nullptr, Log, AddGpuEvent, DeleteGpuEvent, DeliverEvent, nullptr, nullptr, DonateGuestRange, PublishPad};

float g_reportedToneMapPeak = 0.0f;

}

extern "C" {

const HostCallbacks* AstroHybridHostCallbacks_nid_no_patch() {
    return &g_callbacks;
}

void AstroHybridHostHdr_nid_no_patch(AgcDriver::HostHdrStatus& status) {
    status = AgcDriver::HostHdrStatus{};
    status.reportedToneMapPeak = g_reportedToneMapPeak;
}

void AstroHybridNoteReportedToneMapPeak_nid_no_patch(float nits) {
    g_reportedToneMapPeak = nits;
}

}
