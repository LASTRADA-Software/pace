# SPDX-License-Identifier: Apache-2.0
# pace-node — the compile worker for distributed builds.
#
# Unlike pace-cc, this one LINKS the Pace library, and deliberately: it
# is a daemon, and "the same daemon capabilities as pace-kv" means the same
# listener, socket, TLS, logging and (in a later phase) service-management code
# rather than a second implementation of each. The launcher avoids the library
# because it is spawned thousands of times per build and its startup cost is on the
# critical path; a worker starts once and then lives, so the trade is the opposite.
#
# It reuses the launcher's own pieces by compiling those sources in, the same way
# pace-cc compiles in a few Pace leaves. They are the client-side and
# worker-side halves of one protocol, and a second copy of either is how the two
# ends drift apart.
#
# The toolchain-fingerprint group (CmdLine, DirectManifest, Stats,
# ToolchainFingerprint, ToolchainProbe, and AtomicFile, which ToolchainProbe writes
# its cache through) is here for the same reason and is not
# optional: a worker derives its own fingerprint so an operator never has to paste
# a digest, and it MUST derive it through the identical code its clients use. A
# second implementation would register cleanly, heartbeat happily, and never be
# matched -- with nothing anywhere reporting why.
set(_pace_node_shared
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/AtomicFile.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/CacheProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/CmdLine.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/CodecEnvelope.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/CompileJob.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/DirectManifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/Dispatch.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/EndpointDial.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ParallelFor.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ProcessRunner.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/Stats.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/TicketCredentials.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ToolchainDiscovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ToolchainFingerprint.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ToolchainHost.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ToolchainProbe.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/WorkerProtocol.cpp")

add_executable(pace-node
    main.cpp
    NodeLogging.cpp
    AdminEndpoint.cpp
    CacheProxy.cpp
    CacheTier.cpp
    ClusterAdminCli.cpp
    ConsensusTier.cpp
    CordonCli.cpp
    DiscoveryTier.cpp
    EndpointDialer.cpp
    EndpointResolver.cpp
    FleetProbe.cpp
    FleetSummaryResponder.cpp
    ForeignFleetWatch.cpp
    EnrollChannel.cpp
    EnrollClient.cpp
    FormationController.cpp
    FormationEffects.cpp
    FormationLoop.cpp
    FormationRuntime.cpp
    RaftStoreArchiver.cpp
    EnrollmentResponder.cpp
    EnrollmentWindow.cpp
    NodeProofClient.cpp
    NodeProofResponder.cpp
    SchedulerLink.cpp
    SchedulerReachability.cpp
    SchedulerTier.cpp
    SchedulingRedirect.cpp
    SessionResponder.cpp
    ScratchClaim.cpp
    LocalCache.cpp
    HostEventInbox.cpp
    NodeActivation.cpp
    NodeAnnounce.cpp
    NodePresenceTier.cpp
    AdmissionResponder.cpp
    AdmitForwarder.cpp
    MachineBudget.cpp
    NodeRoster.cpp
    NodeConditions.cpp
    NodeConfig.cpp
    NodeDefaults.cpp
    NodeFirewall.cpp
    NodeFormation.cpp
    NodeReload.cpp
    RefusedArguments.cpp
    NodeFrameSurface.cpp
    NodeIdentity.cpp
    NodeKey.cpp
    NodeStateFiles.cpp
    NodeSurfaces.cpp
    NodeToolchains.cpp
    OperatorCredentials.cpp
    RemoteUpstream.cpp
    FrameEndpoint.cpp
    NodeIoLoop.cpp
    WorkerLease.cpp
    WorkerTier.cpp
    CompileCapacity.cpp
    CompileResponder.cpp
    NodeStatusResponder.cpp
    LiveStatsResponder.cpp
    LiveStatsSources.cpp
    FleetReadGate.cpp
    FleetTextResponder.cpp
    SharedCacheTier.cpp
    SharedCacheHost.cpp
    SharedCacheResponder.cpp
    SharedCacheDirectory.cpp
    SharedCacheStatus.cpp
    SharedCacheSession.cpp
    SharedCacheUpstream.cpp
    InProcessSharedUpstream.cpp
    ${_pace_node_shared}
)
target_include_directories(pace-node PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc")
target_link_libraries(pace-node PRIVATE Pace)
target_compile_definitions(pace-node PRIVATE
    PACE_NODE_VERSION="${PACE_VERSION_STRING}")
if(WIN32)
    target_link_libraries(pace-node PRIVATE ws2_32)
endif()

install(TARGETS pace-node
    RUNTIME DESTINATION "${PACE_INSTALL_BINDIR}"
    COMPONENT "${PACE_APP_COMPONENT}"
)

# The node's own unit tests. AdminEndpoint and SchedulerEndpoint are
# the only sources here that the launcher's test binary does not already cover --
# and each of the last two lives outside main.cpp precisely so that it can be.
if(PACE_BUILD_TESTS)
    add_executable(pace-node-tests
        "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/test_main.cpp"
        AdminEndpoint.cpp
        AdminEndpoint_test.cpp
        CacheProxy.cpp
        CacheProxy_test.cpp
        CacheTier.cpp
        CacheTier_test.cpp
        ClusterAdminCli.cpp
        ClusterAdminCli_test.cpp
        ConsensusTier.cpp
        ConsensusTier_test.cpp
        CordonCli.cpp
        CordonCli_test.cpp
        DiscoveryTier.cpp
        DiscoveryTier_test.cpp
        EndpointDialer.cpp
        EndpointResolver.cpp
        EndpointResolver_test.cpp
        FleetProbe.cpp
        FleetProbe_test.cpp
        FleetSummaryResponder.cpp
        FleetSummaryResponder_test.cpp
        ForeignFleetWatch.cpp
        ForeignFleetWatch_test.cpp
        EnrollChannel.cpp
        EnrollChannel_test.cpp
        EnrollClient.cpp
        EnrollClient_test.cpp
        FormationController.cpp
        FormationController_test.cpp
        FormationEffects.cpp
        FormationEffects_test.cpp
        FormationFleet_test.cpp
        FormationLoop.cpp
        FormationLoop_test.cpp
        FormationRuntime.cpp
        FormationRuntime_test.cpp
        RaftStoreArchiver.cpp
        RaftStoreArchiver_test.cpp
        FleetForgetPropagation_test.cpp
        EnrollmentResponder.cpp
        EnrollmentResponder_test.cpp
        EnrollmentWindow.cpp
        EnrollmentWindow_test.cpp
        NodeProofClient.cpp
    NodeProofResponder.cpp
        NodeProofResponder_test.cpp
        ExchangeLog_test.cpp
        HostEventInbox.cpp
        HostEventInbox_test.cpp
        NodeAnnounce_test.cpp
        NodeAudience_test.cpp
        NodePresenceTier_test.cpp
        SchedulerLink.cpp
        SchedulerLink_test.cpp
        SchedulerReachability.cpp
        SchedulerReachability_test.cpp
        SchedulerTier.cpp
        SchedulerTier_test.cpp
        SchedulingRedirect.cpp
        SchedulingRedirect_test.cpp
        SessionResponder.cpp
        SessionResponder_test.cpp
        ScratchClaim.cpp
        ScratchClaim_test.cpp
        LocalCache.cpp
        LocalCache_test.cpp
        RemoteUpstream.cpp
        RemoteUpstream_test.cpp
        OperatorCredentials.cpp
        OperatorCredentials_test.cpp
        NodeActivation.cpp
        NodeActivation_test.cpp
        NodeAnnounce.cpp
        NodePresenceTier.cpp
        AdmissionResponder.cpp
        AdmissionResponder_test.cpp
        AdmitForwarder.cpp
        AdmitForwarder_test.cpp
        MachineBudget.cpp
        MachineBudget_test.cpp
        NodeRoster.cpp
        NodeRoster_test.cpp
        NodeRefusal_test.cpp
        OneShotAnswer_test.cpp
        NodeConditions.cpp
        NodeConditions_test.cpp
        RefusedArguments.cpp
        RefusedArguments_test.cpp
        NodeConfig.cpp
        NodeCredential_test.cpp
        NodeReload.cpp
        NodeReload_test.cpp
        NodeLogging.cpp
        NodeLogging_test.cpp
        NodeFrameSurface.cpp
        NodeFrameSurface_test.cpp
        AuthRefusalContract_test.cpp
        FleetMachinePresence_test.cpp
        FleetRoster_test.cpp
        FleetTickets_test.cpp
        FleetLeaseRouting_test.cpp
        FleetExclusion_test.cpp
        FleetCompileBudget_test.cpp
        FleetDialHint_test.cpp
        FleetVoterMove_test.cpp
        NodeConfig_test.cpp
        NodeDefaults.cpp
        NodeDefaults_test.cpp
        NodeFormation.cpp
        NodeFormation_test.cpp
        NodeIdentity.cpp
        NodeIdentity_test.cpp
        NodeKey.cpp
        NodeKey_test.cpp
        NodeStateFiles.cpp
        NodeStateFiles_test.cpp
        RaftKeyRevocation_test.cpp
        NodeFirewall.cpp
        NodeFirewall_test.cpp
        NodeSurfaces.cpp
        NodeSurfaces_test.cpp
        StartupRefusalCensus_test.cpp
        NodeMembership_test.cpp
        WorkerLease_test.cpp
        NodeToolchains.cpp
        NodeToolchains_test.cpp
        ToolchainHashProgress_test.cpp
        ToolchainWalkConcurrency_test.cpp
        FrameEndpoint.cpp
        NodeIoLoop.cpp
        FrameEndpoint_test.cpp
        WorkerLease.cpp
        WorkerTier.cpp
        WorkerTier_test.cpp
        CompileCapacity.cpp
        CompileCapacity_test.cpp
        CompileResponder.cpp
        NodeStatusResponder.cpp
        LiveStatsResponder.cpp
        LiveStatsSources.cpp
        FleetReadGate.cpp
        FleetTextResponder.cpp
        CompileResponder_test.cpp
        NodeStatusResponder_test.cpp
        LiveStatsResponder_test.cpp
        FleetReadGate_test.cpp
        MembershipGate_test.cpp
        FleetTextResponder_test.cpp
        SharedCacheTier.cpp
        SharedCacheHost.cpp
        SharedCacheHost_test.cpp
        SharedCacheResponder.cpp
        SharedCacheResponder_test.cpp
        SharedCacheDirectory.cpp
        SharedCacheDirectory_test.cpp
        SharedCacheStatus.cpp
        SharedCacheStatus_test.cpp
        SharedCacheSession.cpp
        SharedCacheUpstream.cpp
        SharedCacheUpstream_test.cpp
        FleetSharedCache_test.cpp
        InProcessSharedUpstream.cpp
        InProcessSharedUpstream_test.cpp
        DocumentedCommandLines_test.cpp
        ${_pace_node_shared}
        # The CLI's reading of a `NodeMetrics` body and of a `/metrics` one, compiled in for the
        # reason the launcher's sources are: one case holds the two doors a node's figures leave by
        # against each other, through the code each client reads them with (#1406).
        "${CMAKE_SOURCE_DIR}/src/apps/pacectl/CliAnswer.cpp"
        "${CMAKE_SOURCE_DIR}/src/apps/pacectl/CliValue.cpp"
        "${CMAKE_SOURCE_DIR}/src/apps/pacectl/StatsSource.cpp"
        # The CLI's one reading of a node's conditions, compiled in for the same reason: the case
        # that proves every surface walks the node's real table renders through the code the CLI
        # draws its `node` line and panel with (#1364).
        "${CMAKE_SOURCE_DIR}/src/apps/pacectl/NodeStatusText.cpp"
        # The launcher's reachability memo, so the fleet case can drive the chain from one
        # launcher's unreachable worker to the next one's LEASE through the real scheduler.
        # Tests only: the node never runs the memo.
        "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ReachabilityMemo.cpp"
    )
    target_include_directories(pace-node-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/apps/pace-cc"
        "${CMAKE_SOURCE_DIR}/src/apps/pacectl")
    target_link_libraries(pace-node-tests PRIVATE Pace Catch2::Catch2)
    # The launchd label the macOS uninstaller boots this worker's job out by,
    # handed to the test so it compares the packaging's value against the one the
    # binary derives -- rather than against a literal of its own, which would
    # agree with the code and drift from the package in silence. The uninstaller
    # deletes the executable that would otherwise answer for itself, so a label
    # that drifted leaves a job registered with nothing left able to remove it.
    #
    # The repository root the documentation checks read is NOT a definition here: it
    # comes from <tests/CheckoutPaths.hpp>, included by the cases that read it, so the
    # checkout's path stays out of every other unit's cache key.
    target_compile_definitions(pace-node-tests PRIVATE
        PACE_MACOS_NODE_LABEL="${PACE_MACOS_NODE_LABEL}")
    if(WIN32)
        target_link_libraries(pace-node-tests PRIVATE ws2_32)
    endif()
    include(Catch)
    # `SKIP_RETURN_CODE 4` tells ctest a Catch2 skip is a skip and not a failure
    # (#499), and under the Catch2 the top-level CMakeLists pins EXACTLY it is exact: a
    # run whose every case skipped exits 4 and any failure exits 42, whatever the
    # count. On 3.6.0 the exit code was the failure count, and a case failing four
    # assertions was scored *skipped* (#1128). `catch-skip-return-code` requires the
    # property, and `catch-skip-exit-contract` that it still means what it says.
    catch_discover_tests(pace-node-tests PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION "${PACE_ERROR_DIALOG_ENVIRONMENT}")
endif()
