/* The public headers are a C API: compiled here as C11, so a construct only
 * C++ accepts fails the build. */

#include "mxl-monitoring/content.h"
#include "mxl-monitoring/monitoring.h"
#include "mxl-monitoring/prometheus.h"
#include "mxl-monitoring/reader.h"

/* Referenced from C++ so the object is linked into the tests. */
size_t mxlMonitoringTestCSnapshotSize(void)
{
    return sizeof(mxlMonitoringFlowSnapshot);
}
