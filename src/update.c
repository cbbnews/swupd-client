/*
 *   Software Updater - client side
 *
 *      Copyright © 2012-2016 Intel Corporation.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, version 2 or later of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Authors:
 *         Arjan van de Ven <arjan@linux.intel.com>
 *         Tim Pepper <timothy.c.pepper@linux.intel.com>
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "signature.h"
#include "swupd.h"

int nonpack;

void increment_retries(int *retries, int *timeout)
{
	(*retries)++;
	sleep(*timeout);
	*timeout *= 2;
}

static void try_delta_loop(struct list *updates)
{
	struct list *iter;
	struct file *file;

	iter = list_head(updates);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		if (!file->is_file) {
			continue;
		}

		try_delta(file);
	}
}

static struct list *full_download_loop(struct list *updates, int isfailed)
{
	struct list *iter;
	struct file *file;

	iter = list_head(updates);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		if (file->is_deleted) {
			continue;
		}

		full_download(file);
	}

	if (isfailed) {
		list_free_list(updates);
	}

	return end_full_download();
}

static int stage_content(struct list *updates, struct manifest *manifest)
{
	struct file *file;
	struct list *iter;
	int ret;

	/* starting at list_head in the filename alpha-sorted updates list
	 * means node directories are added before leaf files */
	printf("Staging file content\n");
	iter = list_head(updates);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		if (file->do_not_update || file->is_deleted) {
			continue;
		}

		/* for each file: fdatasync to persist changed content over reboot, or maybe a global sync */
		/* for each file: check hash value; on mismatch delete and queue full download */
		/* todo: hash check */

		ret = do_staging(file, manifest);
		if (ret < 0) {
			printf("File staging failed: %s\n", file->filename);
			return ret;
		}
	}
	return 0;
}

static int update_loop(struct list **updates_list, struct manifest **manifests)
{
	int ret;
	struct list *failed = NULL;
	struct list *updates = updates_list[0];
	struct list *mix_content = updates_list[0];
	struct manifest *server_manifest = manifests[0];
	struct manifest *mix_manifest = manifests[1];
	int err;
	int retries = 0;  /* We only want to go through the download loop once */
	int timeout = 10; /* Amount of seconds for first download retry */

TRY_DOWNLOAD:
	err = start_full_download(true);
	if (err != 0) {
		return err;
	}

	if (failed != NULL) {
		try_delta_loop(failed);
		failed = full_download_loop(failed, 1);
	} else {
		try_delta_loop(updates);
		failed = full_download_loop(updates, 0);
	}

#if 0
	if (rm_staging_dir_contents("download")) {
		return -1;
	}
#endif

	/* Set retries only if failed downloads exist, and only retry a fixed
	   amount of &times */
	if (list_head(failed) != NULL && retries < MAX_TRIES) {
		increment_retries(&retries, &timeout);
		printf("Starting download retry #%d\n", retries);
		clean_curl_multi_queue();
		goto TRY_DOWNLOAD;
	}

	if (retries >= MAX_TRIES) {
		printf("ERROR: Could not download all files, aborting update\n");
		list_free_list(failed);
		return -1;
	}

	/* If mix content exists, download it now only after all the upstream content
	 * was successfully downloaded. Both content will then be staged together. */
	if (mix_content != NULL) {
		set_mix_globals(); /* We must reset the URL to local and local_download = true */
		err = start_full_download(true);
		if (err != 0) {
			return err;
		}

		try_delta_loop(mix_content);
		failed = full_download_loop(mix_content, 0);
		/* There is nothing to retry from with local download */
		if (list_head(failed)) {
			list_free_list(failed);
			return -1;
		}
	}

	if (download_only) {
		return -1;
	}

	/*********** rootfs critical section starts ***************************
         NOTE: the next loop calls do_staging() which can remove files, starting a critical section
	       which ends after rename_all_files_to_final() succeeds
	 */

	/* from here onward we're doing real update work modifying "the disk" */



	/* check policy, and if policy says, "ask", ask the user at this point */
	/* check for reboot need - if needed, wait for reboot */
	ret = stage_content(updates, server_manifest);
	if (ret < 0) {
		return ret;
	}
	if (mix_content != NULL) {
		stage_content(mix_content, mix_manifest);
		if (ret < 0) {
			return ret;
		}
	}

	/* sync */
	sync();

	/* rename to apply update */
	ret = rename_all_files_to_final(updates);
	if (ret != 0) {
		return ret;
	}

	/* TODO: do we need to optimize directory-permission-only changes (directories
	 *       are now sent as tar's so permissions are handled correctly, even
	 *       if less than efficiently)? */

	sync();

	/* NOTE: critical section starts when update_loop() calls do_staging() */
	/*********** critical section ends *************************************/

	return ret;
}

int add_included_manifests(struct manifest *mom, int current, struct list **subs)
{
	struct list *subbed = NULL;
	struct list *iter;
	int ret;

	iter = list_head(*subs);
	while (iter) {
		subbed = list_prepend_data(subbed, ((struct sub *)iter->data)->component);
		iter = iter->next;
	}

	/* Pass the current version here, not the new, otherwise we will never
	 * hit the Manifest delta path. */
	if (add_subscriptions(subbed, subs, current, mom, 0) >= 0) {
		ret = 0;
	} else {
		ret = -1;
	}
	list_free_list(subbed);

	return ret;
}

int setup_mix_update(struct manifest *curr_mix_mom, struct manifest *latest_mix_mom, int *curr_mix_version, int *latest_mix_version, struct list *mix_bundles, struct list *latest_bundles)
{
	int ret;

	read_mix_subscriptions(&mix_bundles);

	ret = check_versions(curr_mix_version, latest_mix_version, path_prefix);
	if (ret < 0) {
		return ret;
	}
	if (*latest_mix_version <= *curr_mix_version) {
		printf("Version on server (%i) is not newer than system version (%i)\n", *latest_mix_version, *curr_mix_version);
		return *curr_mix_version;
	}

	/* No retries here, if it's not on the filesystem, it's not reachable */
	curr_mix_mom = load_mix_mom(*curr_mix_version);
	if (!curr_mix_mom) {
		return EMOM_NOTFOUND;
	}

	latest_mix_mom = load_mix_mom(*latest_mix_version);
	if (!latest_mix_mom) {
		return EMOM_NOTFOUND;
	}

	/* Load current submanifests */
	curr_mix_mom->submanifests = recurse_manifest(curr_mix_mom, mix_bundles, NULL);
	if (!curr_mix_mom->submanifests) {
		fprintf(stderr, "Error: Cannot load mix MoM sub-manifests...continuing without adding mix content\n");
		free_manifest(curr_mix_mom);
		free_manifest(latest_mix_mom);
		return ERECURSE_MANIFEST;
	}
	curr_mix_mom->files = files_from_bundles(curr_mix_mom->submanifests);
	curr_mix_mom->files = consolidate_files(curr_mix_mom->files);

	/* Set subscription versions and link the peers together */
	latest_bundles = list_clone(mix_bundles);
	set_subscription_versions(latest_mix_mom, curr_mix_mom, &latest_bundles);
	link_submanifests(curr_mix_mom, latest_mix_mom, mix_bundles, latest_bundles, false);
	ret = add_included_manifests(latest_mix_mom, *curr_mix_version, &latest_bundles);
	if (ret) {
		free_manifest(curr_mix_mom);
		free_manifest(latest_mix_mom);
		return EMANIFEST_LOAD;
	}

	/* load server submanifests */
	latest_mix_mom->submanifests = recurse_manifest(latest_mix_mom, latest_bundles, NULL);
	if (!latest_mix_mom->submanifests) {
		fprintf(stderr, "Error: Cannot load new mix MoM sub-manifests...continuing without adding mix content\n");
		free_manifest(curr_mix_mom);
		free_manifest(latest_mix_mom);
		return ERECURSE_MANIFEST;
	}
	latest_mix_mom->files = files_from_bundles(latest_mix_mom->submanifests);
	latest_mix_mom->files = consolidate_files(latest_mix_mom->files);

	/* TODO: accounting may need to be seperated from official swupd stats */
	link_manifests(curr_mix_mom, latest_mix_mom);

	ret = download_subscribed_packs(mix_bundles, true);
	if (ret) {
		fprintf(stderr, "Cannot find packs for mix content\n");
		free_manifest(curr_mix_mom);
		free_manifest(latest_mix_mom);
		return ENOSWUPDSERVER;
	}

	return 0;
}

int main_update()
{
	int current_version = -1, server_version = -1;
	int curr_mix_version = -1, latest_mix_version = -1;
	struct manifest *current_manifest = NULL, *server_manifest = NULL;
	struct manifest *curr_mix_mom = NULL, *latest_mix_mom = NULL;
	struct list *updates[2] = {0, 0};
	struct manifest *manifests[2] = {0, 0};
	struct list *current_subs = NULL;
	struct list *latest_subs = NULL;
	struct list *mix_bundles = NULL;
	struct list *latest_bundles = NULL;
	int ret;
	int lock_fd;
	int retries = 0;
	int timeout = 10;
	struct timespec ts_start, ts_stop; // For main swupd update time
	timelist times;
	double delta;

	srand(time(NULL));

	ret = swupd_init(&lock_fd);
	if (ret != 0) {
		/* being here means we already close log by a previously caught error */
		printf("Updater failed to initialize, exiting now.\n");
		return ret;
	}

	times = init_timelist();

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
	grabtime_start(&times, "Main Update");

	if (!check_network()) {
		printf("Error: Network issue, unable to proceed with update\n");
		ret = ENOSWUPDSERVER;
		goto clean_curl;
	}

	printf("Update started.\n");

	grabtime_start(&times, "Update Step 1: get versions");

	read_subscriptions_alt(&current_subs);

	/* Step 1: get versions */

	ret = check_versions(&current_version, &server_version, path_prefix);

	if (ret < 0) {
		ret = EXIT_FAILURE;
		goto clean_curl;
	}
	if (server_version <= current_version) {
		printf("Version on server (%i) is not newer than system version (%i)\n", server_version, current_version);
		ret = EXIT_SUCCESS;
		goto clean_curl;
	}

	printf("Preparing to update from %i to %i\n", current_version, server_version);

	/* Step 2: housekeeping */

	if (rm_staging_dir_contents("download")) {
		printf("Error cleaning download directory\n");
		ret = EXIT_FAILURE;
		goto clean_curl;
	}
	grabtime_stop(&times);
	grabtime_stop(&times); // Close step 1
	grabtime_start(&times, "Load Manifests:");
load_current_mom:
	/* Step 3: setup manifests */

	/* get the from/to MoM manifests */
	current_manifest = load_mom(current_version);
	if (!current_manifest) {
		/* TODO: possibly remove this as not getting a "from" manifest is not fatal
		 * - we just don't apply deltas */
		if (retries < MAX_TRIES) {
			increment_retries(&retries, &timeout);
			printf("Retry #%d downloading from/to MoM Manifests\n", retries);
			goto load_current_mom;
		}
		printf("Failure retrieving manifest from server\n");
		ret = EMOM_NOTFOUND;
		goto clean_exit;
	}

	/*  Reset the retries and timeout for subsequent download calls */
	retries = 0;
	timeout = 10;

load_server_mom:
	grabtime_stop(&times); // Close step 2
	grabtime_start(&times, "Recurse and Consolidate Manifests");
	server_manifest = load_mom(server_version);
	if (!server_manifest) {
		if (retries < MAX_TRIES) {
			increment_retries(&retries, &timeout);
			printf("Retry #%d downloading server Manifests\n", retries);
			goto load_server_mom;
		}
		printf("Failure retrieving manifest from server\n");
		printf("Unable to load manifest after retrying (config or network problem?)\n");
		ret = EMOM_NOTFOUND;
		goto clean_exit;
	}

	retries = 0;
	timeout = 10;

load_current_submanifests:
	/* Read the current collective of manifests that we are subscribed to.
	 * First load up the old (current) manifests. Statedir could have been cleared
	 * or corrupt, so don't assume things are already there. Updating subscribed
	 * manifests is done as part of recurse_manifest */
	current_manifest->submanifests = recurse_manifest(current_manifest, current_subs, NULL);
	if (!current_manifest->submanifests) {
		if (retries < MAX_TRIES) {
			increment_retries(&retries, &timeout);
			printf("Retry #%d downloading current sub-manifests\n", retries);
			goto load_current_submanifests;
		}
		ret = ERECURSE_MANIFEST;
		printf("Cannot load current MoM sub-manifests, exiting\n");
		goto clean_exit;
	}
	retries = 0;
	timeout = 10;

	/* consolidate the current collective manifests down into one in memory */
	current_manifest->files = files_from_bundles(current_manifest->submanifests);

	current_manifest->files = consolidate_files(current_manifest->files);

	latest_subs = list_clone(current_subs);
	set_subscription_versions(server_manifest, current_manifest, &latest_subs);
	link_submanifests(current_manifest, server_manifest, current_subs, latest_subs, false);
	/* The new subscription is seeded from the list of currently installed bundles
	 * This calls add_subscriptions which recurses for new includes */
	grabtime_start(&times, "Add Included Manifests");
	ret = add_included_manifests(server_manifest, current_version, &latest_subs);
	grabtime_stop(&times);
	if (ret) {
		ret = EMANIFEST_LOAD;
		goto clean_exit;
	}

load_server_submanifests:
	/* read the new collective of manifests that we are subscribed to in the new MoM */
	server_manifest->submanifests = recurse_manifest(server_manifest, latest_subs, NULL);
	if (!server_manifest->submanifests) {
		if (retries < MAX_TRIES) {
			increment_retries(&retries, &timeout);
			printf("Retry #%d downloading server sub-manifests\n", retries);
			goto load_server_submanifests;
		}
		ret = ERECURSE_MANIFEST;
		printf("Error: Cannot load server MoM sub-manifests, exiting\n");
		goto clean_exit;
	}
	retries = 0;
	timeout = 10;
	/* consolidate the new collective manifests down into one in memory */
	server_manifest->files = files_from_bundles(server_manifest->submanifests);

	server_manifest->files = consolidate_files(server_manifest->files);

	set_subscription_versions(server_manifest, current_manifest, &latest_subs);
	link_submanifests(current_manifest, server_manifest, current_subs, latest_subs, true);

	/* prepare for an update process based on comparing two in memory manifests */
	link_manifests(current_manifest, server_manifest);
#if 0
	debug_write_manifest(current_manifest, "debug_manifest_current.txt");
	debug_write_manifest(server_manifest, "debug_manifest_server.txt");
#endif
	grabtime_stop(&times);
	/* Step 4: check disk state before attempting update */
	grabtime_start(&times, "Pre-Update Scripts");
	run_preupdate_scripts(server_manifest);
	grabtime_stop(&times);

	grabtime_start(&times, "Download Packs");

download_packs:
	/* Step 5: get the packs and untar */
	ret = download_subscribed_packs(latest_subs, false);
	if (ret) {
		// packs don't always exist, tolerate that but not ENONET
		if (retries < MAX_TRIES) {
			increment_retries(&retries, &timeout);
			printf("Retry #%d downloading packs\n", retries);
			goto download_packs;
		}
		printf("No network, or server unavailable for pack downloads\n");
		ret = ENOSWUPDSERVER;
		goto clean_exit;
	}
	grabtime_stop(&times);
	grabtime_start(&times, "Create Update List");
	/* Step 6: some more housekeeping */
	/* TODO: consider trying to do less sorting of manifests */

	updates[0] = create_update_list(current_manifest, server_manifest);
	manifests[0] = server_manifest;

	link_renames(updates[0], current_manifest); /* TODO: Have special lists for candidate and renames */

	print_statistics(current_version, server_version);
	grabtime_stop(&times);
	/* Step 7: apply the update */

	/*
	 * need update list in filename order to insure directories are
	 * created before their contents
	 */
	grabtime_start(&times, "Update Loop");
	updates[0] = list_sort(updates[0], file_sort_filename);

	/* Yell loudly if things fail here...but don't kill the regular update if local content fails to add */
	if (check_mix_exists()) {
		ret = setup_mix_update(curr_mix_mom, latest_mix_mom, &curr_mix_version, &latest_mix_version, mix_bundles, latest_bundles);
		if (ret == 0) {
			updates[1] = create_update_list(curr_mix_mom, latest_mix_mom);
			updates[1] = list_sort(updates[1], file_sort_filename);
			manifests[1] = latest_mix_mom;
		}
	}

	ret = update_loop(updates, manifests);
	if (ret == 0) {
		/* Failure to write the version file in the state directory
		 * should not affect exit status. */
		(void)update_device_latest_version(server_version);
		printf("Update was applied.\n");
	} else if (ret < 0) {
		// Ensure a positive exit status for the main program.
		ret = -ret;
	}

	delete_motd();
	grabtime_stop(&times);
	/* Run any scripts that are needed to complete update */
	grabtime_start(&times, "Run Scripts");
	run_scripts();
	grabtime_stop(&times);

clean_exit:
	list_free_list(updates[0]);
	free_manifest(current_manifest);
	free_manifest(server_manifest);

clean_curl:
	grabtime_stop(&times);
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts_stop);
	delta = ts_stop.tv_sec - ts_start.tv_sec + ts_stop.tv_nsec / 1000000000.0 - ts_start.tv_nsec / 1000000000.0;
	telemetry(ret ? TELEMETRY_CRIT : TELEMETRY_INFO,
		  "update",
		  "current_version=%d\n"
		  "server_version=%d\n"
		  "result=%d\n"
		  "time=%5.1f\n",
		  current_version,
		  server_version,
		  ret,
		  delta);

	if (server_version > current_version) {
		printf("Update took %0.1f seconds\n", delta);
	}
	print_time_stats(&times);

	swupd_deinit(lock_fd, &latest_subs);

	if ((current_version < server_version) && (ret == 0)) {
		printf("Update successful. System updated from version %d to version %d\n",
		       current_version, server_version);
	} else if (ret == 0) {
		printf("Update complete. System already up-to-date at version %d\n", current_version);
	}

	if (nonpack > 0) {
		printf("%i files were not in a pack\n", nonpack);
	}

	return ret;
}
