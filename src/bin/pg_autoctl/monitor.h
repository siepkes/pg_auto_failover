/*
 * src/bin/pg_autoctl/monitor.h
 *     Functions for interacting with a pg_auto_failover monitor
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>

#include "pgsql.h"
#include "monitor_config.h"
#include "primary_standby.h"
#include "state.h"

/* the monitor manages a postgres server running the pgautofailover extension */
typedef struct Monitor
{
	PGSQL pgsql;
	PGSQL notificationClient;
	MonitorConfig config;
} Monitor;

typedef struct MonitorAssignedState
{
	char name[_POSIX_HOST_NAME_MAX];
	int nodeId;
	int groupId;
	NodeState state;
	int candidatePriority;
	bool replicationQuorum;
} MonitorAssignedState;

typedef struct StateNotification
{
	char message[BUFSIZE];
	NodeState reportedState;
	NodeState goalState;
	char formationId[NAMEDATALEN];
	int groupId;
	int nodeId;
	char hostName[_POSIX_HOST_NAME_MAX];
	int nodePort;
} StateNotification;

typedef struct MonitorExtensionVersion
{
	char defaultVersion[BUFSIZE];
	char installedVersion[BUFSIZE];
} MonitorExtensionVersion;

bool monitor_init(Monitor *monitor, char *url);
void monitor_setup_notifications(Monitor *monitor, int groupId, int nodeId);
bool monitor_has_received_notifications(Monitor *monitor);
bool monitor_process_state_notification(int notificationGroupId,
										int notificationNodeId,
										char *channel,
										char *payload);
bool monitor_local_init(Monitor *monitor);
void monitor_finish(Monitor *monitor);

bool monitor_retryable_error(const char *sqlstate);

bool monitor_get_nodes(Monitor *monitor, char *formation, int groupId,
					   NodeAddressArray *nodeArray);
bool monitor_print_nodes(Monitor *monitor, char *formation, int groupId);
bool monitor_print_nodes_as_json(Monitor *monitor, char *formation, int groupId);
bool monitor_get_other_nodes(Monitor *monitor,
							 int myNodeId,
							 NodeState currentState,
							 NodeAddressArray *nodeArray);
bool monitor_print_other_nodes(Monitor *monitor,
							   int myNodeId, NodeState currentState);
bool monitor_print_other_nodes_as_json(Monitor *monitor,
									   int myNodeId,
									   NodeState currentState);

bool monitor_get_primary(Monitor *monitor, char *formation, int groupId,
						 NodeAddress *node);
bool monitor_get_coordinator(Monitor *monitor, char *formation,
							 NodeAddress *node);
bool monitor_get_most_advanced_standby(Monitor *monitor,
									   char *formation, int groupId,
									   NodeAddress *node);
bool monitor_register_node(Monitor *monitor,
						   char *formation,
						   char *name,
						   char *host,
						   int port,
						   uint64_t system_identifier,
						   char *dbname,
						   int desiredNodeId,
						   int desiredGroupId,
						   NodeState initialState,
						   PgInstanceKind kind,
						   int candidatePriority,
						   bool quorum,
						   char *citusClusterName,
						   bool *mayRetry,
						   MonitorAssignedState *assignedState);
bool monitor_node_active(Monitor *monitor,
						 char *formation, int nodeId,
						 int groupId, NodeState currentState,
						 bool pgIsRunning,
						 char *currentLSN, char *pgsrSyncState,
						 MonitorAssignedState *assignedState);
bool monitor_get_node_replication_settings(Monitor *monitor,
										   NodeReplicationSettings *settings);
bool monitor_set_node_candidate_priority(Monitor *monitor,
										 char *formation, char *name,
										 int candidatePriority);
bool monitor_set_node_replication_quorum(Monitor *monitor,
										 char *formation, char *name,
										 bool replicationQuorum);
bool monitor_get_formation_number_sync_standbys(Monitor *monitor, char *formation,
												int *numberSyncStandbys);
bool monitor_set_formation_number_sync_standbys(Monitor *monitor, char *formation,
												int numberSyncStandbys);

bool monitor_remove(Monitor *monitor, char *host, int port);

bool monitor_count_groups(Monitor *monitor, char *formation, int *groupsCount);
bool monitor_get_groupId_from_name(Monitor *monitor,
								   char *formation, char *name,
								   int *groupId);

bool monitor_perform_failover(Monitor *monitor, char *formation, int group);
bool monitor_perform_promotion(Monitor *monitor, char *formation, char *name);

bool monitor_print_state(Monitor *monitor, char *formation, int group);
bool monitor_print_last_events(Monitor *monitor,
							   char *formation, int group, int count);
bool monitor_print_state_as_json(Monitor *monitor, char *formation, int group);
bool monitor_print_last_events_as_json(Monitor *monitor,
									   char *formation, int group,
									   int count,
									   FILE *stream);

bool monitor_print_every_formation_uri(Monitor *monitor, const SSLOptions *ssl);
bool monitor_print_every_formation_uri_as_json(Monitor *monitor,
											   const SSLOptions *ssl,
											   FILE *stream);

bool monitor_create_formation(Monitor *monitor, char *formation, char *kind,
							  char *dbname, bool ha, int numberSyncStandbys);
bool monitor_enable_secondary_for_formation(Monitor *monitor,
											const char *formation);
bool monitor_disable_secondary_for_formation(Monitor *monitor,
											 const char *formation);

bool monitor_drop_formation(Monitor *monitor, char *formation);

bool monitor_count_failover_candidates(Monitor *monitor,
									   char *formation, int groupId,
									   int *failoverCandidateCount);

bool monitor_print_formation_settings(Monitor *monitor, char *formation);
bool monitor_print_formation_settings_as_json(Monitor *monitor, char *formation);

bool monitor_formation_uri(Monitor *monitor,
						   const char *formation,
						   const char *citusClusterName,
						   const SSLOptions *ssl,
						   char *connectionString,
						   size_t size);

bool monitor_synchronous_standby_names(Monitor *monitor,
									   char *formation, int groupId,
									   char *synchronous_standby_names,
									   int size);

bool monitor_update_node_metadata(Monitor *monitor,
								  int nodeId,
								  const char *name,
								  const char *hostname,
								  int port);
bool monitor_set_node_system_identifier(Monitor *monitor,
										int nodeId,
										uint64_t system_identifier);
bool monitor_set_group_system_identifier(Monitor *monitor,
										 int groupId,
										 uint64_t system_identifier);

bool monitor_start_maintenance(Monitor *monitor, int nodeId, bool *mayRetry);
bool monitor_stop_maintenance(Monitor *monitor, int nodeId, bool *mayRetry);

bool monitor_get_notifications(Monitor *monitor, int timeoutMs);
bool monitor_wait_until_primary_applied_settings(Monitor *monitor,
												 const char *formation);
bool monitor_wait_until_some_node_reported_state(Monitor *monitor,
												 const char *formation,
												 int groupId,
												 PgInstanceKind nodeKind,
												 NodeState targetState,
												 int timeout);
bool monitor_wait_until_node_reported_state(Monitor *monitor,
											const char *formation,
											int groupId,
											int nodeId,
											PgInstanceKind nodeKind,
											NodeState targetState);
bool monitor_wait_for_state_change(Monitor *monitor,
								   const char *formation,
								   int groupId,
								   int nodeId,
								   int timeoutMs,
								   bool *stateHasChanged);
bool monitor_get_extension_version(Monitor *monitor,
								   MonitorExtensionVersion *version);
bool monitor_extension_update(Monitor *monitor, const char *targetVersion);
bool monitor_ensure_extension_version(Monitor *monitor,
									  LocalPostgresServer *postgres,
									  MonitorExtensionVersion *version);


#endif /* MONITOR_H */
