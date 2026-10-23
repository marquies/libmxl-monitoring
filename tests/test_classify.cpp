// Every libmxl status, and what it means for an open attempt, a read and a
// write. A status libmxl adds later falls to OTHER or ERROR until it is
// decided here.

#include <catch2/catch_test_macros.hpp>
#include "mxl-monitoring/monitoring.h"

extern "C" size_t mxlMonitoringTestCSnapshotSize(void);

namespace
{
    struct Row
    {
        mxlStatus status;
        mxlMonitoringAccess access;
        mxlMonitoringRead read;
        mxlMonitoringWrite write;
    };

    constexpr Row Table[] = {
        {MXL_STATUS_OK,                  MXL_MONITORING_ACCESS_OPEN,              MXL_MONITORING_READ_OK,                MXL_MONITORING_WRITE_OK               },
        {MXL_ERR_UNKNOWN,                MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_FLOW_NOT_FOUND,         MXL_MONITORING_ACCESS_FLOW_NOT_FOUND,    MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_OUT_OF_RANGE_TOO_LATE,  MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_TOO_LATE,          MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_OUT_OF_RANGE_TOO_EARLY, MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_TOO_EARLY,         MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INVALID_FLOW_READER,    MXL_MONITORING_ACCESS_FLOW_INVALID,      MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INVALID_FLOW_WRITER,    MXL_MONITORING_ACCESS_FLOW_INVALID,      MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_TIMEOUT,                MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_TOO_EARLY,         MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INVALID_ARG,            MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_CONFLICT,               MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_PERMISSION_DENIED,      MXL_MONITORING_ACCESS_PERMISSION_DENIED, MXL_MONITORING_READ_PERMISSION_DENIED, MXL_MONITORING_WRITE_PERMISSION_DENIED},
        {MXL_ERR_FLOW_INVALID,           MXL_MONITORING_ACCESS_FLOW_INVALID,      MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_STRLEN,                 MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INTERRUPTED,            MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_NO_FABRIC,              MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INVALID_STATE,          MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_INTERNAL,               MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_NOT_READY,              MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_NOT_FOUND,              MXL_MONITORING_ACCESS_FLOW_NOT_FOUND,    MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_EXISTS,                 MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
        {MXL_ERR_UNSUPPORTED_OPERATION,  MXL_MONITORING_ACCESS_OTHER,             MXL_MONITORING_READ_ERROR,             MXL_MONITORING_WRITE_ERROR            },
    };
}

TEST_CASE("Every libmxl status is classified for access, reads and writes", "[classify]")
{
    for (auto const& row : Table)
    {
        INFO("status " << static_cast<int>(row.status));
        CHECK(mxlMonitoringClassifyAccess(row.status) == row.access);
        CHECK(mxlMonitoringClassifyRead(row.status) == row.read);
        CHECK(mxlMonitoringClassifyWrite(row.status) == row.write);
    }
}

TEST_CASE("C and C++ see the same snapshot layout", "[classify]")
{
    CHECK(mxlMonitoringTestCSnapshotSize() == sizeof(mxlMonitoringFlowSnapshot));
}
