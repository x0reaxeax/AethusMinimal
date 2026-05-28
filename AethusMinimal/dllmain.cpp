/*
 * AethusMinimal - Placement restrictions removal for AETHUS (UE5)
 *  https://github.com/x0reaxeax/AethusMinimal
 *
 * SDK Dependencies
 * =======================================
 *  - SDK/Basic.hpp                                 -> Offsets::ProcessEventIdx,
 *                                                     InSDKUtils::GetImageBase(), GetVirtualFunction()
 *  - SDK/Engine_classes.hpp                        -> UObject, UFunction
 *  - SDK/BP_Placeable_Ghost_BASE_parameters.hpp    -> param structs
 * 
**/

#include <Windows.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <algorithm>

// Disabled globally in project settings (SDK gen'd code spits these)
// #pragma warning (disable : 4309) // 'initializing': truncation of constant value
// #pragma warning (disable : 4369) // enumeration values overflow underlying type

#include "SDK/Basic.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/BP_Placeable_Ghost_BASE_parameters.hpp"

#include "MinHook/include/MinHook.h"

// -------------------------------------------------------------------------
// Main toggle
// -------------------------------------------------------------------------
bool g_bFreePlacementEnabled = true;

// -------------------------------------------------------------------------
// void UObject::ProcessEvent(UFunction* Function, void* Parms)
// -------------------------------------------------------------------------
using ProcessEvent_t = void(__thiscall*)(SDK::UObject*, SDK::UFunction*, void*);
ProcessEvent_t oProcessEvent = nullptr;

// -------------------------------------------------------------------------
// Hook
// -------------------------------------------------------------------------
void __fastcall hk_ProcessEvent(
    SDK::UObject* This, 
    SDK::UFunction* Function, 
    void* Parms
) {
    // One-time confirmation the hook is firing.
    static bool bFirstCall = true;
    if (bFirstCall) { 
        bFirstCall = false; 
        printf("[AethusMinimal::ProcessEvent] Hooked ProcessEvent is firing..\n"); 
    }

    if (nullptr == Function) {
        oProcessEvent(This, Function, Parms);
        return;
    }

    const std::string szFuncName = Function->GetName();

    // -------------------------------------------------------------------------
    // Ghost free placement:
    //
    // ReceiveTick fires every frame on the ghost actor via ProcessEvent.
    // Inside it, `DoValidityChecks` (and all sub-checks) run as internal
    // `EX_LocalFinalFunction` calls – they never surface in ProcessEvent.
    //
    // Approach:
    //   1. Before tick: zero `CollisionCount` + disable `CollisionChecksEnabled`
    //      so `DoCollisionChecks` always sees zero overlaps.
    //   2. Run original `ReceiveTick` (all internal checks fire, then
    //      `SetActorPlaceableState` is called internally to set ghost red).
    //   3. After tick: call `SetActorPlaceableState(0)` to force valid/green (or blue ig) state.
    //
    // `ReceiveActorBeginOverlap` / `EndOverlap`: let them run for other side-effects
    // but immediately reset `CollisionCount` to 0 so it never accumulates.
    //
    // Ghost actor layout (BP_Placeable_Ghost_BASE_C):
    //   0x05B0  int32  CollisionCount          -- (> 0) means collision check fails    [ override = 0 ]
    //   0x05B4  bool   MustPlaceInHabitat      -- (true) means habitat check fails     [ override = 0 ]
    //   0x05B5  bool   MustPlaceOutsideHabitat -- (true) means exterior check fails    [ override = 0 ]
    //   0x05F0  uint8  CurrentPlaceableState   -- E_PlaceableState
    //   0x0610  bool   CollisionChecksEnabled
    // -------------------------------------------------------------------------
    if (
        g_bFreePlacementEnabled && 
        nullptr != This && 
        nullptr != This->Class
    ) {
        const std::string szClassName = This->Class->GetName();
        if (szClassName.find("Ghost") != std::string::npos) {
            const uintptr_t qwGhostBase = reinterpret_cast<uintptr_t>(This);

            if ("ReceiveTick" == szFuncName) {
                // Pre-tick: suppress collision checks
                *reinterpret_cast<int32_t*>(qwGhostBase + 0x05B0) = 0;      // CollisionCount
                *reinterpret_cast<bool*>   (qwGhostBase + 0x0610) = false;  // CollisionChecksEnabled

                // Suppress habitat/exterior placement restrictions.
                // `DoHabitatPlacementChecks` reads `MustPlaceInHabitat` and
                // `DoExteriorPlacementChecks` reads `MustPlaceOutsideHabitat`.
                // Zeroing both means neither check can fail, and no error
                // message is built/displayed for either case.
                *reinterpret_cast<bool*>(qwGhostBase + 0x05B4) = false;     // MustPlaceInHabitat
                *reinterpret_cast<bool*>(qwGhostBase + 0x05B5) = false;     // MustPlaceOutsideHabitat

                oProcessEvent(This, Function, Parms);

                // Post-tick: force valid state via SetActorPlaceableState(0)
                static SDK::UFunction* lpSetStateFn = nullptr;
                if (nullptr == lpSetStateFn) {
                    lpSetStateFn = This->Class->GetFunction(
                        "BP_Placeable_Ghost_BASE_C", 
                        "SetActorPlaceableState"
                    );
                }

                if (nullptr != lpSetStateFn) {
                    alignas(16) uint8_t stateParms[0x0188] = {};            // State=0 (NewEnumerator0) @ [0x0000]
                    oProcessEvent(This, lpSetStateFn, stateParms);
                } else {
                    // Fallback: direct write if function lookup failed
                    *reinterpret_cast<uint8_t*>(qwGhostBase + 0x05F0) = 0;  // CurrentPlaceableState
                }
                return;
            }

            if ("ReceiveActorBeginOverlap" == szFuncName || "ReceiveActorEndOverlap" == szFuncName) {
                oProcessEvent(This, Function, Parms);                       // run for other side-effects
                *reinterpret_cast<int32_t*>(qwGhostBase + 0x05B0) = 0;      // reset CollisionCount
                return;
            }
        }
    }

    oProcessEvent(This, Function, Parms);
}

// -------------------------------------------------------------------------
// Install / remove
// -------------------------------------------------------------------------
static bool InstallFreePlacementHook(void) {
    LPVOID lpProcessEvent = reinterpret_cast<void*>(
        SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent
    );

    printf(
        "[AethusMinimal] Installing hook at ProcessEvent (0x%llX)\n",
        reinterpret_cast<unsigned long long>(lpProcessEvent)
    );

    MH_STATUS mhStatus = MH_Initialize();
    if (MH_OK != mhStatus) {
        fprintf(stderr, 
            "[AethusMinimal] MH_Initialize failed: %s\n", 
            MH_StatusToString(mhStatus)
        );
        return false;
    }

    mhStatus = MH_CreateHook(
        lpProcessEvent,
        reinterpret_cast<void*>(&hk_ProcessEvent),
        reinterpret_cast<void**>(&oProcessEvent)
    );

    if (MH_OK != mhStatus) {
        fprintf(
            stderr,
            "[AethusMinimal] MH_CreateHook failed: %s\n", 
            MH_StatusToString(mhStatus)
        );
        return false;
    }

    mhStatus = MH_EnableHook(lpProcessEvent);
    if (MH_OK != mhStatus) {
        fprintf(
            stderr,
            "[AethusMinimal] MH_EnableHook failed: %s\n", 
            MH_StatusToString(mhStatus)
        );
        return false;
    }

    fprintf(
        stderr,
        "[AethusMinimal] Hook installed at ProcessEvent (0x%llX)\n",
        reinterpret_cast<unsigned long long>(lpProcessEvent)
    );

    return true;
}

static void RemoveFreePlacementHook(void) {
    LPVOID lpProcessEvent = reinterpret_cast<void*>(
        SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent
    );

    MH_DisableHook(lpProcessEvent);
    MH_RemoveHook(lpProcessEvent);
    MH_Uninitialize();

    oProcessEvent = nullptr;
}


DWORD WINAPI ThreadEntry(LPVOID lpParameter) {
#if defined(_DEBUG)
    FILE* fpStdout, * fpStderr;
    AllocConsole();

    freopen_s(&fpStdout, "CONOUT$", "w", stdout);
    freopen_s(&fpStderr, "CONOUT$", "w", stderr);
#endif 

    printf("[AethusMinimal] Initializing..\n");

    // Wait for GObjects as a proxy for "game is initialized" before patching bytes.
    while (!SDK::UObject::GObjects)
        Sleep(100);

    printf("[AethusMinimal] Free placement: %s\n", g_bFreePlacementEnabled ? "ON" : "OFF");
    g_bFreePlacementEnabled = true;

    if (!InstallFreePlacementHook()) {
        printf("[AethusMinimal] Failed to install free placement hook.\n");
#ifndef _DEBUG
        MessageBoxA(NULL, "Failed to install free placement hook.", "AethusMinimal", MB_ICONERROR);
#endif // !_DEBUG
        return EXIT_FAILURE;
    }

    printf("[AethusMinimal] Free placement hook installed successfully.\n");
    
    return EXIT_SUCCESS;
}

BOOL APIENTRY DllMain(
    HMODULE hModule, 
    DWORD ul_reason_for_call, 
    LPVOID lpReserved
) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, ThreadEntry, nullptr, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            RemoveFreePlacementHook();
            break;
    }
    return TRUE;
}