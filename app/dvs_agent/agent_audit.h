#pragma once
#include <json-c/json.h>

#include <string>

/* Bounded in-memory audit ring (AG-7): config/network/maintenance/
 * engine-link outcomes stay queryable via /media/logs and /audit-events.
 * No persistence; reboot clears it and that is honest (volatile op log). */
void agent_audit(const std::string& scope, const std::string& action,
                 const std::string& result, const std::string& detail);

/* Newest `limit` entries (0 = all), chronological. New reference, caller puts. */
json_object* agent_audit_snapshot(size_t limit);

/* Page for /audit-events: page>=1, pageSize clamped by caller. Items are the
 * same chronological slice the old handler served. New reference. */
json_object* agent_audit_page(int page, int page_size, int* total_out);
