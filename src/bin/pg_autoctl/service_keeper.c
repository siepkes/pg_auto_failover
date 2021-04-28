/*
 * src/bin/pg_autoctl/service_keeper.c
 *   The main loop of the pg_autoctl keeper
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "cli_common.h"
#include "cli_root.h"
#include "defaults.h"
#include "fsm.h"
#include "keeper.h"
#include "keeper_config.h"
#include "keeper_pg_init.h"
#include "log.h"
#include "monitor.h"
#include "pgctl.h"
#include "pidfile.h"
#include "service_keeper.h"
#include "service_postgres_ctl.h"
#include "signals.h"
#include "state.h"
#include "string_utils.h"
#include "supervisor.h"

#include "runprogram.h"

static bool keepRunning = true;

/* list of hooks to run at reload time */
KeeperReloadFunction KeeperReloadHooksArray[] = {
	&keeper_reload_configuration,
	NULL
};

KeeperReloadFunction *KeeperReloadHooks = KeeperReloadHooksArray;

static bool keeper_node_active(Keeper *keeper, bool doInit);
static void check_for_network_partitions(Keeper *keeper);
static bool is_network_healthy(Keeper *keeper);
static bool in_network_partition(KeeperStateData *keeperState, uint64_t now,
								 int networkPartitionTimeout);


/*
 * keeper_service_start starts the keeper processes: the node_active main loop
 * and depending on the current state the Postgres instance.
 */
bool
start_keeper(Keeper *keeper)
{
	const char *pidfile = keeper->config.pathnames.pid;

	Service subprocesses[] = {
		{
			.name = SERVICE_NAME_POSTGRES,
			.policy = RP_PERMANENT,
			.leader = false,
			.pid = -1,
			.startFunction = &service_postgres_ctl_start
		},
		{
			.name = SERVICE_NAME_KEEPER,
			.policy = RP_PERMANENT,
			.leader = true,
			.pid = -1,
			.startFunction = &service_keeper_start,
			.context = (void *) keeper
		}
	};

	int subprocessesCount = sizeof(subprocesses) / sizeof(subprocesses[0]);

	return supervisor_start(subprocesses, subprocessesCount, pidfile);
}


/*
 * keeper_start_node_active_process starts a sub-process that communicates with
 * the monitor to implement the node_active protocol.
 */
bool
service_keeper_start(void *context, pid_t *pid)
{
	Keeper *keeper = (Keeper *) context;

	/* Flush stdio channels just before fork, to avoid double-output problems */
	fflush(stdout);
	fflush(stderr);

	/* time to create the node_active sub-process */
	pid_t fpid = fork();

	switch (fpid)
	{
		case -1:
		{
			log_error("Failed to fork the node-active process");
			return false;
		}

		case 0:
		{
			/* here we call execv() so we never get back */
			(void) service_keeper_runprogram(keeper);

			/* unexpected */
			log_fatal("BUG: returned from service_keeper_runprogram()");
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		default:
		{
			/* fork succeeded, in parent */
			log_debug("pg_autoctl node-active process started in subprocess %d",
					  fpid);
			*pid = fpid;
			return true;
		}
	}
}


/*
 * service_keeper_runprogram runs the node_active protocol service:
 *
 *   $ pg_autoctl do service node-active --pgdata ...
 *
 * This function is intended to be called from the child process after a fork()
 * has been successfully done at the parent process level: it's calling
 * execve() and will never return.
 */
void
service_keeper_runprogram(Keeper *keeper)
{
	char *args[12];
	int argsIndex = 0;

	char command[BUFSIZE];

	/*
	 * use --pgdata option rather than the config.
	 *
	 * On macOS when using /tmp, the file path is then redirected to being
	 * /private/tmp when using realpath(2) as we do in normalize_filename(). So
	 * for that case to be supported, we explicitely re-use whatever PGDATA or
	 * --pgdata was parsed from the main command line to start our sub-process.
	 */
	char *pgdata = keeperOptions.pgSetup.pgdata;
	IntString semIdString = intToString(log_semaphore.semId);

	setenv(PG_AUTOCTL_DEBUG, "1", 1);
	setenv(PG_AUTOCTL_LOG_SEMAPHORE, semIdString.strValue, 1);

	args[argsIndex++] = (char *) pg_autoctl_program;
	args[argsIndex++] = "do";
	args[argsIndex++] = "service";
	args[argsIndex++] = "node-active";
	args[argsIndex++] = "--pgdata";
	args[argsIndex++] = pgdata;
	args[argsIndex++] = logLevelToString(log_get_level());
	args[argsIndex] = NULL;

	/* we do not want to call setsid() when running this program. */
	Program program = initialize_program(args, false);

	program.capture = false;    /* redirect output, don't capture */
	program.stdOutFd = STDOUT_FILENO;
	program.stdErrFd = STDERR_FILENO;

	/* log the exact command line we're using */
	(void) snprintf_program_command_line(&program, command, BUFSIZE);

	log_info("%s", command);

	(void) execute_program(&program);
}


/*
 * service_keeper_node_active_init initializes the pg_autoctl service for the
 * node_active protocol.
 */
bool
service_keeper_node_active_init(Keeper *keeper)
{
	KeeperConfig *config = &(keeper->config);

	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;
	bool monitorDisabledIsOk = true;

	if (!keeper_config_read_file(config,
								 missingPgdataIsOk,
								 pgIsNotRunningIsOk,
								 monitorDisabledIsOk))
	{
		/* errors have already been logged. */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	/*
	 * Check that the init is finished. This function is called from
	 * cli_service_run when used in the CLI `pg_autoctl run`, and the
	 * function cli_service_run calls into keeper_init(): we know that we could
	 * read a keeper state file.
	 */
	if (file_exists(config->pathnames.init))
	{
		log_warn("The `pg_autoctl create` did not complete, completing now.");

		if (!keeper_pg_init_continue(keeper))
		{
			/* errors have already been logged. */
			return false;
		}
	}

	if (!keeper_init(keeper, config))
	{
		log_fatal("Failed to initialize keeper, see above for details");
		exit(EXIT_CODE_PGCTL);
	}

	return true;
}


/*
 * keeper_node_active_loop implements the main loop of the keeper, which
 * periodically gets the goal state from the monitor and makes the state
 * transitions.
 */
bool
keeper_node_active_loop(Keeper *keeper, pid_t start_pid)
{
	Monitor *monitor = &(keeper->monitor);
	KeeperConfig *config = &(keeper->config);
	KeeperStateData *keeperState = &(keeper->state);
	LocalPostgresServer *postgres = &(keeper->postgres);

	bool doSleep = false;
	bool couldContactMonitor = false;
	bool firstLoop = true;
	bool doInit = true;
	bool warnedOnCurrentIteration = false;
	bool warnedOnPreviousIteration = false;

	bool shutdownSequenceInProgress = false;
	bool couldStartMaintenanceAtShutdown = false;
	bool reachedMaintenanceAtShutdown = false;

	bool shutdownInMaintenance =
		keeperState->service_state == SERVICE_SHUTDOWN_IN_MAINTENANCE;

	bool disabledMaintenanceAtStartup = false;

	log_debug("pg_autoctl service is starting");

	keeperState->service_state = SERVICE_STARTUP;

	/* setup our monitor client connection with our notification handler */
	(void) monitor_setup_notifications(monitor,
									   keeperState->current_group,
									   keeperState->current_node_id);

	while (keepRunning)
	{
		bool couldContactMonitorThisRound = false;

		bool needStateChange = false;
		bool transitionFailed = false;

		/*
		 * If we're in a stable state (current state and goal state are the
		 * same, and this didn't change in the previous loop), then we can
		 * sleep for a while. As the monitor notifies every state change, we
		 * can also interrupt our sleep as soon as we get the hint.
		 */
		if (doSleep && !config->monitorDisabled)
		{
			int timeoutMs = PG_AUTOCTL_KEEPER_SLEEP_TIME * 1000;

			bool groupStateHasChanged = false;

			/* establish a connection for notifications if none present */
			(void) pgsql_prepare_to_wait(&(monitor->notificationClient));
			(void) monitor_wait_for_state_change(monitor,
												 config->formation,
												 keeperState->current_group,
												 keeperState->current_node_id,
												 timeoutMs,
												 &groupStateHasChanged);

			/* when no state change has been notified, close the connection */
			if (!groupStateHasChanged &&
				monitor->notificationClient.connectionStatementType ==
				PGSQL_CONNECTION_MULTI_STATEMENT)
			{
				pgsql_finish(&(monitor->notificationClient));
			}
		}
		else if (doSleep && config->monitorDisabled)
		{
			int timeoutUs = PG_AUTOCTL_KEEPER_SLEEP_TIME * 1000 * 1000;

			pg_usleep(timeoutUs);
		}

		doSleep = true;

		/*
		 * Handle signals.
		 *
		 * When asked to STOP, we always finish the current transaction before
		 * doing so, which means we only check if asked_to_stop at the
		 * beginning of the loop.
		 *
		 * We have several places where it's safe to check if SIGQUIT has been
		 * signaled to us and from where we can immediately exit whatever we're
		 * doing. It's important to avoid e.g. leaving state.new files behind.
		 */
		if (asked_to_reload || firstLoop)
		{
			(void) keeper_call_reload_hooks(keeper, firstLoop, doInit);
		}

		if (asked_to_stop_fast || asked_to_quit)
		{
			/*
			 * Refrain from editing service_state when shutdown is already in
			 * progress.
			 */
			if (!shutdownSequenceInProgress)
			{
				shutdownSequenceInProgress = true;
				keeperState->service_state = SERVICE_SHUTDOWNING;
			}
			break;
		}

		/* Check that we still own our PID file, or quit now */
		(void) check_pidfile(config->pathnames.pid, start_pid);

		CHECK_FOR_FAST_SHUTDOWN;

		/*
		 * Read the current state. While we could preserve the state in memory,
		 * re-reading the file simplifies recovery from failures. For example,
		 * if we fail to write the state file after making a transition, then
		 * we should not tell the monitor that the transition succeeded, because
		 * a subsequent crash of the keeper would cause the states to become
		 * inconsistent. By re-reading the file, we make sure the state on disk
		 * on the keeper is consistent with the state on the monitor
		 *
		 * Also, when --disable-monitor is used, then we get our assigned state
		 * by reading the state file, which is edited by an external process.
		 */
		if (!keeper_load_state(keeper))
		{
			log_error("Failed to read keeper state file, retrying...");
			CHECK_FOR_FAST_SHUTDOWN;
			continue;
		}

		/*
		 * Upon receiving SIGTERM, start maintenance on the monitor, possibly
		 * triggering a failover when the primary node is being shut down.
		 */
		if (asked_to_stop)
		{
			/*
			 * When the monitor is disabled, SIGTERM is the same as SIGINT.
			 */
			if (config->monitorDisabled)
			{
				shutdownSequenceInProgress = true;
				keeperState->service_state = SERVICE_SHUTDOWNING;

				break;
			}
			/* Shutdown Sequence in progress, and maintenance enabled */
			else if (shutdownSequenceInProgress &&
					 couldStartMaintenanceAtShutdown)
			{
				log_trace("Shutdown in progress");
			}
			/* Trigger the shutdown sequence and enable maintenance now */
			else if (keeperState->current_role != MAINTENANCE_STATE &&
					 keeperState->current_role != PREPARE_MAINTENANCE_STATE)
			{
				int nodeId = keeper->state.current_node_id;
				bool mayRetry = false;

				log_info("Enabling maintenance on this node for shutdown");

				/*
				 * Make sure that at restart, we get out of maintenance, even
				 * if we later receive a stronger signal such as SIGINT
				 * (asked_to_stop_fast) or SIGQUIT (asked_to_quit).
				 */
				keeperState->service_state = SERVICE_SHUTDOWN_IN_MAINTENANCE;

				if (keeper_store_state(keeper) &&
					monitor_start_maintenance(monitor, nodeId, &mayRetry))
				{
					couldStartMaintenanceAtShutdown = true;
				}
				else
				{
					/* warning: we're going to try again */
					log_warn("Failed to start maintenance "
							 "upon receiving SIGTERM");
				}
			}

			/* in all cases, shutdown is now in progress */
			shutdownSequenceInProgress = true;
		}

		if (firstLoop)
		{
			log_info("pg_autoctl service is running, "
					 "current state is \"%s\"",
					 NodeStateToString(keeperState->current_role));

			keeperState->service_state = SERVICE_RUNNING;
		}

		/*
		 * Check for any changes in the local PostgreSQL instance, and update
		 * our in-memory values for the replication WAL lag and sync_state.
		 */
		if (!keeper_update_pg_state(keeper))
		{
			warnedOnCurrentIteration = true;
			log_warn("Failed to update the keeper's state from the local "
					 "PostgreSQL instance.");
		}
		else if (warnedOnPreviousIteration)
		{
			log_info("Updated the keeper's state from the local "
					 "PostgreSQL instance, which is %s",
					 postgres->pgIsRunning ? "running" : "not running");
		}

		CHECK_FOR_FAST_SHUTDOWN;

		/*
		 * If the monitor is disabled, read the list of other nodes from our
		 * file on-disk at config->pathnames.nodes. The following command can
		 * be used to fill-in that file:
		 *
		 *  $ pg_autoctl do fsm nodes set nodes.json
		 */
		if (config->monitorDisabled)
		{
			/* force cache invalidation when reaching WAIT_STANDBY */
			bool forceCacheInvalidation =
				keeperState->current_role == WAIT_STANDBY_STATE;

			/* maybe update our cached list of other nodes */
			if (!keeper_refresh_other_nodes(keeper, forceCacheInvalidation))
			{
				/* we will try again... */
				log_warn("Failed to update our list of other nodes");
				continue;
			}
		}
		/*
		 * If the monitor is not disabled, call the node_active function on the
		 * monitor and update the keeper data structure accordingy, refreshing
		 * our cache of other nodes if needed.
		 */
		else
		{
			couldContactMonitorThisRound = keeper_node_active(keeper, doInit);

			if (!couldContactMonitor &&
				couldContactMonitorThisRound &&
				!firstLoop)
			{
				/*
				 * Last message the user saw in the output is the following,
				 * and so we should say that we're back to the expected
				 * situation:
				 *
				 * Failed to get the goal state from the monitor
				 */
				log_info("Successfully got the goal state from the monitor");
			}

			couldContactMonitor = couldContactMonitorThisRound;
		}

		if (keeperState->assigned_role != keeperState->current_role)
		{
			needStateChange = true;

			if (couldContactMonitor)
			{
				log_info("Monitor assigned new state \"%s\"",
						 NodeStateToString(keeperState->assigned_role));
			}
			else
			{
				/* if network is not healthy we might self-assign a state */
				log_info("Reaching new state \"%s\"",
						 NodeStateToString(keeperState->assigned_role));
			}
		}

		CHECK_FOR_FAST_SHUTDOWN;

		/*
		 * If we see that PostgreSQL is not running when we know it should be,
		 * the least we can do is start PostgreSQL again. Same if PostgreSQL is
		 * running and we are DEMOTED, or in another one of those states where
		 * the monitor asked us to stop serving queries, in order to ensure
		 * consistency.
		 *
		 * Only enfore current state when we have a recent enough version of
		 * it, meaning that we could contact the monitor.
		 *
		 * We need to prevent the keeper from restarting PostgreSQL at boot
		 * time when meanwhile the Monitor did set our goal_state to DEMOTED
		 * because the other node has been promoted, which could happen if this
		 * node was rebooting for a long enough time.
		 */
		if (needStateChange)
		{
			/*
			 * First, ensure the current state (make sure Postgres is running
			 * if it should, or Postgres is stopped if it should not run).
			 *
			 * The transition function we call next might depend on our
			 * assumption that Postgres is running in the current state.
			 */
			if (keeper_should_ensure_current_state_before_transition(keeper))
			{
				if (!keeper_ensure_current_state(keeper))
				{
					/*
					 * We don't take care of the warnedOnCurrentIteration here
					 * because the real thing that should happen is the
					 * transition to the next state. That's what we keep track
					 * of with "transitionFailed".
					 */
					log_warn(
						"pg_autoctl failed to ensure current state \"%s\": "
						"PostgreSQL %s running",
						NodeStateToString(keeperState->current_role),
						postgres->pgIsRunning ? "is" : "is not");
				}
			}

			if (!keeper_fsm_reach_assigned_state(keeper))
			{
				log_error("Failed to transition to state \"%s\", retrying... ",
						  NodeStateToString(keeperState->assigned_role));

				transitionFailed = true;
			}
		}
		else if (couldContactMonitor || config->monitorDisabled)
		{
			if (!keeper_ensure_current_state(keeper))
			{
				warnedOnCurrentIteration = true;
				log_warn("pg_autoctl failed to ensure current state \"%s\": "
						 "PostgreSQL %s running",
						 NodeStateToString(keeperState->current_role),
						 postgres->pgIsRunning ? "is" : "is not");
			}
			else if (warnedOnPreviousIteration)
			{
				log_info("pg_autoctl managed to ensure current state \"%s\": "
						 "PostgreSQL %s running",
						 NodeStateToString(keeperState->current_role),
						 postgres->pgIsRunning ? "is" : "is not");
			}
		}

		/*
		 * When we receive SIGTERM, we enable maintenance on the monitor, and
		 * we record this action in the Keeper State file. At startup, we then
		 * disable maintenance again. We might have to retry disabling
		 * maintenance should it fail.
		 *
		 * The shutdown sequence might also have been interrupted in
		 * wait_maintenance state, in which case we want to first finish the
		 * FSM transition to maintenance, and only later disable maintenance.
		 */
		if (shutdownInMaintenance &&
			!disabledMaintenanceAtStartup &&
			keeperState->assigned_role == MAINTENANCE_STATE &&
			keeperState->current_role == keeperState->assigned_role &&
			!needStateChange)
		{
			int nodeId = keeper->state.current_node_id;
			bool mayRetry = false;

			log_info("pg_autoctl node was put in maintenance as part of "
					 "shutting down, now disabling maintenance");

			if (monitor_stop_maintenance(monitor, nodeId, &mayRetry))
			{
				disabledMaintenanceAtStartup = true;
			}
			else
			{
				/* errors have already been logged */
				log_debug("Failed to disable maintenance at startup");
			}
		}

		/* now is a good time to make sure we're closing our connections */
		pgsql_finish(&(postgres->sqlClient));

		CHECK_FOR_FAST_SHUTDOWN;

		/*
		 * Write the current (changed) state to disk.
		 *
		 * When using a monitor, even if a transition failed, we still write
		 * the state file to update timestamps used for the network partition
		 * checks.
		 *
		 * When the monitor is disabled, only write the state to disk when we
		 * just successfully implemented a state change.
		 */
		if (!config->monitorDisabled || (needStateChange && !transitionFailed))
		{
			if (!keeper_store_state(keeper))
			{
				transitionFailed = true;
			}
		}

		if ((needStateChange ||
			 (!config->monitorDisabled &&
			  monitor_has_received_notifications(monitor))) &&
			!transitionFailed)
		{
			/* cycle faster if we made a state transition */
			doSleep = false;
		}

		/*
		 * On SIGTERM, we call pgautofailover.start_maintenance() on the
		 * monitor and wait until fully registered in maintenance mode before
		 * shutting down this process, and having the supervisor then shutdown
		 * Postgres.
		 */
		if (shutdownSequenceInProgress)
		{
			/* are we completely done? */
			if (keeperState->assigned_role == MAINTENANCE_STATE &&
				keeperState->current_role == keeperState->assigned_role &&
				couldStartMaintenanceAtShutdown &&
				reachedMaintenanceAtShutdown)
			{
				keepRunning = false;
				log_info("Shutdown sequence complete: reached state \"%s\"",
						 NodeStateToString(MAINTENANCE_STATE));
			}
			/*
			 * When reaching maintenance, we need another loop to call
			 * node_active() on the monitor. Register that we reached the
			 * maintenance state locally so that we can complete shutdown at
			 * next iteration.
			 */
			else if (keeperState->assigned_role == MAINTENANCE_STATE &&
					 keeperState->current_role == keeperState->assigned_role)
			{
				reachedMaintenanceAtShutdown = true;
			}
			/* shutdown still in progress (wait_maintenance, etc) */
			else
			{
				log_debug("Shutdown sequence in progress: "
						  "waiting until state \"%s\" is reached, now at %s/%s",
						  NodeStateToString(MAINTENANCE_STATE),
						  NodeStateToString(keeperState->current_role),
						  NodeStateToString(keeperState->assigned_role));
			}
		}

		if (asked_to_stop_fast)
		{
			keepRunning = false;
		}

		if (firstLoop)
		{
			firstLoop = false;
		}

		/* if we failed to contact the monitor, we must re-try the init steps */
		if (doInit && couldContactMonitorThisRound)
		{
			doInit = false;
		}

		/*
		 * On the first loop, we might have reload-time actions to implement
		 * before and after having contacted the monitor. For instance,
		 * contacting the monitor might show that we're not a primary anymore
		 * after having been DEMOTED during a failover, while this node was
		 * rebooting or something.
		 *
		 * So in some cases, we want to do two rounds of start-up reload:
		 *
		 *   reload-hook(firstLoop => true, doInit => true)
		 *   reload-hook(firstLoop => true, doInit => false)
		 *
		 * Later SIGHUP signal processing will trigger a call to our reload
		 * hooks with both firstLoop and doInit false, and that's handled
		 * earlier in this loop.
		 */
		if (firstLoop)
		{
			(void) keeper_call_reload_hooks(keeper, firstLoop, doInit);
		}

		/* advance the warnings "counters" */
		if (warnedOnPreviousIteration)
		{
			warnedOnPreviousIteration = false;
		}

		if (warnedOnCurrentIteration)
		{
			warnedOnPreviousIteration = true;
			warnedOnCurrentIteration = false;
		}
	}

	/* One last check that we do not have any connections open */
	pgsql_finish(&(keeper->monitor.pgsql));

	/*
	 * When shutdown is already in progress, we might have a service_state set
	 * to SERVICE_SHUTDOWN_IN_MAINTENANCE and we want to keep that. That said,
	 * CHECK_FOR_FAST_SHUTDOWN is a straight break to this point at the end of
	 * the loop, so the state might still be SERVICE_RUNNING.
	 *
	 * Other case is having receveived SIGINT (asked_to_stop_fast) and then we
	 * registered SERVICE_SHUTDOWNING, and now is the time to clear that up and
	 * set SERVICE_SHUTDOWNED instead.
	 */
	if (!shutdownSequenceInProgress ||
		keeperState->service_state == SERVICE_RUNNING ||
		keeperState->service_state == SERVICE_SHUTDOWNING)
	{
		keeperState->service_state = SERVICE_SHUTDOWNED;
	}

	if (!keeper_store_state(keeper))
	{
		/* errors have already been logged */
	}

	return true;
}


/*
 * keeper_node_active calls the node_active function on the monitor, and when
 * it could contact the monitor it also updates our copy of the list of other
 * nodes currenty in the group (keeper->otherNodes).
 *
 * keeper_node_active returns true if it could successfully connect to the
 * monitor, and false otherwise. When it returns false, it also checks for
 * network partitions and set the goal state to DEMOTE_TIMEOUT_STATE when
 * needed.
 */
static bool
keeper_node_active(Keeper *keeper, bool doInit)
{
	KeeperConfig *config = &(keeper->config);
	KeeperStateData *keeperState = &(keeper->state);
	Monitor *monitor = &(keeper->monitor);
	LocalPostgresServer *postgres = &(keeper->postgres);

	uint64_t now = time(NULL);
	MonitorAssignedState assignedState = { 0 };

	char expectedSlotName[BUFSIZE] = { 0 };

	bool forceCacheInvalidation = false;
	bool reportPgIsRunning = ReportPgIsRunning(keeper);

	/*
	 * First, connect to the monitor and check we're compatible with the
	 * extension there. An upgrade on the monitor might have happened in
	 * between loops here.
	 *
	 * Note that we don't need a very strong a guarantee about the version
	 * number of the monitor extension, as we have other places in the code
	 * that are protected against "suprises". The worst case would be a race
	 * condition where the extension check passes, and then the monitor is
	 * upgraded, and then we call node_active().
	 *
	 *  - The extension on the monitor is protected against running a version
	 *    of the node_active (or any other) function that does not match with
	 *    the SQL level version.
	 *
	 *  - Then, if we changed the API without changing the arguments, that
	 *    means we changed what we may return. We are protected against changes
	 *    in number of return values, so we're left with changes within the
	 *    columns themselves. Basically that's a new state that we don't know
	 *    how to handle. In that case we're going to fail to parse it, and at
	 *    next attempt we're going to catch up with the new version number.
	 *
	 * All in all, the worst case is going to be one extra call before we
	 * restart node active process, and an extra error message in the logs
	 * during the live upgrade of pg_auto_failover.
	 */
	if (!keeper_check_monitor_extension_version(keeper))
	{
		/*
		 * We could fail here for two different reasons:
		 *
		 * - if we failed to connect to the monitor (network split, monitor is
		 *   in maintenance or being restarted, etc): in that case just return
		 *   false and have the main loop handle the situation
		 *
		 * - if we could connect to the monitor and then failed to check that
		 *   the version of the monitor is the one we expect, then we're not
		 *   compatible with this monitor and that's a different story.
		 */
		if (monitor->pgsql.status != PG_CONNECTION_OK)
		{
			/*
			 * Check whether we're likely to be in a network partition. That
			 * will cause the assigned_role to become demoted.
			 */
			(void) check_for_network_partitions(keeper);

			return false;
		}

		/*
		 * Okay we're not compatible with the current version of the
		 * pgautofailover extension on the monitor. The most plausible scenario
		 * is that the monitor got update: we're still running e.g. 1.4 and the
		 * monitor is running 1.5.
		 *
		 * In that case we exit, and because the keeper node-active service is
		 * RP_PERMANENT the supervisor is going to restart this process. The
		 * restart happens with fork() and exec(), so it uses the current
		 * version of pg_autoctl binary on disk, which with luck has been
		 * updated to e.g. 1.5 too.
		 *
		 * TL;DR: just exit now, have the service restarted by the supervisor
		 * with the expected version of pg_autoctl that matches the monitor's
		 * extension version.
		 */
		exit(EXIT_CODE_MONITOR);
	}

	if (doInit)
	{
		PostgresSetup *pgSetup = &(postgres->postgresSetup);
		uint64_t system_identifier = pgSetup->control.system_identifier;

		if (!monitor_set_group_system_identifier(monitor,
												 keeperState->current_group,
												 system_identifier))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/* We used to output that in INFO every 5s, which is too much chatter */
	log_debug("Calling node_active for node %s/%d/%d with current state: "
			  "%s, "
			  "PostgreSQL %s running, "
			  "sync_state is \"%s\", "
			  "current lsn is \"%s\".",
			  config->formation,
			  keeperState->current_node_id,
			  keeperState->current_group,
			  NodeStateToString(keeperState->current_role),
			  reportPgIsRunning ? "is" : "is not",
			  postgres->pgsrSyncState,
			  postgres->currentLSN);


	/* ensure we use the correct retry policy with the monitor */
	(void) pgsql_set_main_loop_retry_policy(&(monitor->pgsql.retryPolicy));

	/*
	 * Report the current state to the monitor and get the assigned state.
	 */
	if (!monitor_node_active(monitor,
							 config->formation,
							 keeperState->current_node_id,
							 keeperState->current_group,
							 keeperState->current_role,
							 reportPgIsRunning,
							 postgres->currentLSN,
							 postgres->pgsrSyncState,
							 &assignedState))
	{
		log_error("Failed to get the goal state from the monitor");

		/*
		 * Check whether we're likely to be in a network partition.
		 * That will cause the assigned_role to become demoted.
		 */
		(void) check_for_network_partitions(keeper);

		return false;
	}

	/*
	 * We could contact the monitor, update our internal state.
	 */
	keeperState->last_monitor_contact = now;
	keeperState->assigned_role = assignedState.state;

	if (keeperState->assigned_role != keeperState->current_role)
	{
		log_debug("keeper_node_active: %s ➜ %s",
				  NodeStateToString(keeperState->current_role),
				  NodeStateToString(keeperState->assigned_role));
	}

	/* maybe update our cached list of other nodes */
	if (!keeper_refresh_other_nodes(keeper, forceCacheInvalidation))
	{
		/*
		 * We have a new MD5 but failed to update our list, try again next
		 * round, the monitor might be restarting or something.
		 */
		log_error("Failed to update our list of other nodes");
		return false;
	}

	/*
	 * Also update the groupId and replication slot name in the
	 * configuration file.
	 */
	(void) postgres_sprintf_replicationSlotName(assignedState.nodeId,
												expectedSlotName,
												sizeof(expectedSlotName));

	if (assignedState.groupId != config->groupId ||
		strneq(config->replication_slot_name, expectedSlotName))
	{
		bool postgresNotRunningIsOk = false;

		if (!keeper_config_update(config,
								  assignedState.nodeId,
								  assignedState.groupId))
		{
			log_error("Failed to update the configuration file "
					  "with groupId %d and replication.slot \"%s\"",
					  assignedState.groupId, expectedSlotName);
			return false;
		}

		if (!keeper_ensure_configuration(keeper, postgresNotRunningIsOk))
		{
			log_error("Failed to update our Postgres configuration "
					  "after a change of groupId or "
					  "replication slot name, see above for details");
			return false;
		}
	}

	return true;
}


/*
 * check_for_network_partitions checks whether we're likely to be in a network
 * partition. That will cause the assigned_role to become demoted.
 */
static void
check_for_network_partitions(Keeper *keeper)
{
	KeeperStateData *keeperState = &(keeper->state);

	if (keeperState->current_role == PRIMARY_STATE)
	{
		log_warn("Checking for network partitions...");

		if (!is_network_healthy(keeper))
		{
			keeperState->assigned_role = DEMOTE_TIMEOUT_STATE;

			log_info("Network in not healthy, switching to state %s",
					 NodeStateToString(keeperState->assigned_role));
		}
		else
		{
			log_info("Network is healthy");
		}
	}
}


/*
 * is_network_healthy returns false if the keeper appears to be in a
 * network partition, which it assumes to be the case if it cannot
 * communicate with neither the monitor, nor the secondary for at least
 * network_partition_timeout seconds.
 *
 * On the other side of the network partition, the monitor and the secondary
 * may proceed with a failover once the network partition timeout has passed,
 * since they are sure the primary is down at that point.
 */
static bool
is_network_healthy(Keeper *keeper)
{
	KeeperConfig *config = &(keeper->config);
	KeeperStateData *keeperState = &(keeper->state);
	LocalPostgresServer *postgres = &(keeper->postgres);
	int networkPartitionTimeout = config->network_partition_timeout;
	uint64_t now = time(NULL);
	bool hasReplica = false;

	if (keeperState->current_role != PRIMARY_STATE)
	{
		/*
		 * Fail-over may only occur if we're currently the primary, so
		 * we don't need to check for network partitions in other states.
		 */
		return true;
	}

	if (primary_has_replica(postgres, PG_AUTOCTL_REPLICA_USERNAME, &hasReplica) &&
		hasReplica)
	{
		keeperState->last_secondary_contact = now;
		log_warn("We lost the monitor, but still have a standby: "
				 "we're not in a network partition, continuing.");
		return true;
	}

	if (!in_network_partition(keeperState, now, networkPartitionTimeout))
	{
		/* still had recent contact with monitor and/or secondary */
		return true;
	}

	log_info("Failed to contact the monitor or standby in %d seconds, "
			 "at %d seconds we shut down PostgreSQL to prevent split brain issues",
			 (int) (now - keeperState->last_monitor_contact),
			 networkPartitionTimeout);

	return false;
}


/*
 * in_network_partition determines if we're in a network partition by applying
 * the configured network_partition_timeout to current known values. Updating
 * the state before calling this function is advised.
 */
static bool
in_network_partition(KeeperStateData *keeperState, uint64_t now,
					 int networkPartitionTimeout)
{
	uint64_t monitor_contact_lag = (now - keeperState->last_monitor_contact);
	uint64_t secondary_contact_lag = (now - keeperState->last_secondary_contact);

	return keeperState->last_monitor_contact > 0 &&
		   keeperState->last_secondary_contact > 0 &&
		   networkPartitionTimeout < monitor_contact_lag &&
		   networkPartitionTimeout < secondary_contact_lag;
}
