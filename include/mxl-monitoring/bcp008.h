// What an AMWA BCP-008 monitor reports for a flow: an NcReceiverMonitor for a
// reader or replication source, an NcSenderMonitor for a writer or
// replication target. Worked out from snapshots, with no NMOS dependency.
//
// The IS-12 implementation of the Node holds the monitor object. It applies
// statusReportingDelay, counts status transitions, derives overallStatus,
// and makes a monitor Inactive on deactivation; the calls here decide the
// domain statuses it is handed and the counters it answers with.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "monitoring.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // The values BCP-008 gives every status domain. Link status uses 1 to 3
    // for AllUp, SomeDown and AllDown, synchronization 0 for NotUsed.
    typedef enum mxlMonitoringBcp008Status
    {
        // Nothing since the previous snapshot decides the status: the monitor
        // keeps reporting what it reported.
        MXL_MONITORING_BCP008_UNCHANGED = -1,
        MXL_MONITORING_BCP008_INACTIVE = 0,
        MXL_MONITORING_BCP008_HEALTHY = 1,
        MXL_MONITORING_BCP008_PARTIALLY_HEALTHY = 2,
        MXL_MONITORING_BCP008_UNHEALTHY = 3,
    } mxlMonitoringBcp008Status;

    typedef struct mxlMonitoringBcp008Domain_t
    {
        mxlMonitoringBcp008Status status;
        // Why, for the domain's status message; "" when healthy.
        char message[MXL_MONITORING_TEXT];
    } mxlMonitoringBcp008Domain;

    typedef struct mxlMonitoringBcp008Statuses_t
    {
        uint32_t version; // 1
        uint32_t size;    // sizeof(mxlMonitoringBcp008Statuses)

        // The MXL domain is the flow's only link: AllDown while it is
        // unavailable, AllUp otherwise.
        mxlMonitoringBcp008Domain link;
        // Receiver: connectionStatus. Sender: transmissionStatus.
        mxlMonitoringBcp008Domain connection;
        // NotUsed: MXL timestamps come from the node's own clock.
        mxlMonitoringBcp008Domain synchronization;
        // Receiver: streamStatus. Sender: essenceStatus.
        mxlMonitoringBcp008Domain stream;
    } mxlMonitoringBcp008Statuses;

    // The domain statuses of the flow in current, judged on what was counted
    // since previous, the snapshot of the same flow at the previous poll.
    // previous may be NULL, for a first judgement, which then rests on the
    // flow's access and delivery alone. After a restart of the process
    // counting (see mxlMonitoringBcp008Restarted) every count of current is
    // taken as counted since previous. MXL_ERR_INVALID_ARG where a snapshot
    // is not version 2 or previous is another flow or role.
    //
    // A disconnected flow is Inactive. Otherwise, for a receiver:
    //   connection  Unhealthy while the flow is not open or delivers nothing,
    //               or if since previous it went away, stalled, failed a
    //               read, lost grains or samples (skipped, missing) or read
    //               late (too early past the grace); Healthy otherwise
    //   stream      by the worst content issue read since previous:
    //               Unhealthy for wrong format, wrong size or invalid,
    //               PartiallyHealthy for wrong resolution, wrong rate,
    //               incomplete or illegal values, Healthy for none;
    //               UNCHANGED if nothing but missing grains was read
    // and for a sender:
    //   connection  Unhealthy while the flow is not open or nothing is
    //               committed, or if since previous it went away, stalled
    //               or a commit failed; Healthy otherwise
    //   stream      Unhealthy if since previous input was invalid, or every
    //               write lacked input in time; PartiallyHealthy if some
    //               did; Healthy otherwise; UNCHANGED if nothing was written
    mxlStatus mxlMonitoringBcp008GetStatuses(mxlMonitoringFlowSnapshot const* previous, mxlMonitoringFlowSnapshot const* current,
        mxlMonitoringBcp008Statuses* statuses);

    // Whether the process counting the flow started again between two
    // snapshots of it: any count is lower. Never within one process; a Node
    // polling another process's counts checks every poll and, on a restart,
    // drops the baseline of mxlMonitoringBcp008GetCounters for NULL, since
    // counts compared with a baseline from before the restart are wrong as
    // soon as they climb past it.
    bool mxlMonitoringBcp008Restarted(mxlMonitoringFlowSnapshot const* previous, mxlMonitoringFlowSnapshot const* current);

    typedef enum mxlMonitoringBcp008Counters
    {
        MXL_MONITORING_BCP008_LOST,               // receiver: GetLostPacketCounters
        MXL_MONITORING_BCP008_LATE,               // receiver: GetLatePacketCounters
        MXL_MONITORING_BCP008_TRANSMISSION_ERROR, // sender: GetTransmissionErrorCounters
    } mxlMonitoringBcp008Counters;

    typedef struct mxlMonitoringBcp008Counter_t
    {
        char const* name;        // static
        char const* description; // static
        uint64_t value;
    } mxlMonitoringBcp008Counter;

    // The counters of one kind for the flow in current, counted since
    // baseline, the snapshot taken at the monitor's last reset, or since the
    // process started for NULL. With any count lower than in baseline, every
    // count of current is taken as counted since baseline. Fills up to
    // *count counters and sets *count to the number there are: for a
    // receiver, lost holds skipped and missing in grains or samples, late
    // holds too_early in reads; for a sender, transmission errors hold
    // permission_denied and write_error in failed commits and out_of_space
    // in failed attempts to write. A kind the flow's side does not have has
    // none, which BCP-008 answers with an empty collection.
    // MXL_ERR_INVALID_ARG where a snapshot is not version 2 or baseline is
    // another flow or role.
    mxlStatus mxlMonitoringBcp008GetCounters(mxlMonitoringFlowSnapshot const* baseline, mxlMonitoringFlowSnapshot const* current,
        mxlMonitoringBcp008Counters which, mxlMonitoringBcp008Counter* counters, size_t* count);

#ifdef __cplusplus
}
#endif
