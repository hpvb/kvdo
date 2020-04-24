/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournal.c#56 $
 */

#include "recoveryJournal.h"
#include "recoveryJournalInternals.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "constants.h"
#include "dataVIO.h"
#include "extent.h"
#include "header.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalBlock.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "waitQueue.h"

struct recovery_journal_state_7_0 {
	/** Sequence number to start the journal */
	sequence_number_t journal_start;
	/** Number of logical blocks used by VDO */
	block_count_t logical_blocks_used;
	/** Number of block map pages allocated */
	block_count_t block_map_data_blocks;
} __attribute__((packed));

static const struct header RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = RECOVERY_JOURNAL,
	.version =
		{
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

static const uint64_t RECOVERY_COUNT_MASK = 0xff;

enum {
	/*
	 * The number of reserved blocks must be large enough to prevent a
	 * new recovery journal block write from overwriting a block which
	 * appears to still be a valid head block of the journal. Currently,
	 * that means reserving enough space for all 2048 VIOs, or 8 blocks.
	 */
	RECOVERY_JOURNAL_RESERVED_BLOCKS = 8,
};

/**********************************************************************/
const char *get_journal_operation_name(journal_operation operation)
{
	switch (operation) {
	case DATA_DECREMENT:
		return "data decrement";

	case DATA_INCREMENT:
		return "data increment";

	case BLOCK_MAP_DECREMENT:
		return "block map decrement";

	case BLOCK_MAP_INCREMENT:
		return "block map increment";

	default:
		return "unknown journal operation";
	}
}

/**
 * Get a block from the end of the free list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static struct recovery_journal_block *
pop_free_list(struct recovery_journal *journal)
{
	return block_from_ring_node(popRingNode(&journal->free_tail_blocks));
}

/**
 * Get a block from the end of the active list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static struct recovery_journal_block *
pop_active_list(struct recovery_journal *journal)
{
	return block_from_ring_node(popRingNode(&journal->active_tail_blocks));
}

/**
 * Assert that we are running on the journal thread.
 *
 * @param journal        The journal
 * @param function_name  The function doing the check (for logging)
 **/
static void assert_on_journal_thread(struct recovery_journal *journal,
				     const char *function_name)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() == journal->thread_id),
			"%s() called on journal thread", function_name);
}

/**
 * waiter_callback implementation invoked whenever a data_vio is to be released
 * from the journal, either because its entry was committed to disk,
 * or because there was an error.
 **/
static void continue_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("$F($j-$js);cb=continueJournalWaiter($j-$js)"));
	int wait_result = *((int *)context);
	continue_data_vio(data_vio, wait_result);
}

/**
 * Check whether the journal has any waiters on any blocks.
 *
 * @param journal  The journal in question
 *
 * @return <code>true</code> if any block has a waiter
 **/
static inline bool has_block_waiters(struct recovery_journal *journal)
{
	// Either the first active tail block (if it exists) has waiters,
	// or no active tail block has waiters.
	if (isRingEmpty(&journal->active_tail_blocks)) {
		return false;
	}

	struct recovery_journal_block *block =
		block_from_ring_node(journal->active_tail_blocks.next);
	return (has_waiters(&block->entry_waiters)
		|| has_waiters(&block->commit_waiters));
}

/**********************************************************************/
static void recycle_journal_block(struct recovery_journal_block *block);
static void notify_commit_waiters(struct recovery_journal *journal);

/**
 * Check whether the journal has drained.
 *
 * @param journal The journal which may have just drained
 **/
static void check_for_drain_complete(struct recovery_journal *journal)
{
	int result = VDO_SUCCESS;
	if (is_read_only(journal->read_only_notifier)) {
		result = VDO_READ_ONLY;
		/*
		 * Clean up any full active blocks which were not written due to
		 * being in read-only mode.
		 *
		 * XXX: This would probably be better as a short-circuit in
		 * write_block().
		 */
		notify_commit_waiters(journal);

		// Release any DataVIOs waiting to be assigned entries.
		notify_all_waiters(&journal->decrement_waiters, continue_waiter,
				   &result);
		notify_all_waiters(&journal->increment_waiters, continue_waiter,
				   &result);
	}

	if (!is_draining(&journal->state) || journal->reaping
	    || has_block_waiters(journal)
	    || has_waiters(&journal->increment_waiters)
	    || has_waiters(&journal->decrement_waiters)) {
		return;
	}

	if (is_saving(&journal->state)) {
		if (journal->active_block != NULL) {
			ASSERT_LOG_ONLY(((result == VDO_READ_ONLY)
					 || !is_recovery_block_dirty(journal->active_block)),
					"journal being saved has clean active block");
			recycle_journal_block(journal->active_block);
		}

		ASSERT_LOG_ONLY(isRingEmpty(&journal->active_tail_blocks),
				"all blocks in a journal being saved must be inactive");
	}

	finish_draining_with_result(&journal->state, result);
}

/**
 * Notifiy a recovery journal that the VDO has gone read-only.
 *
 * <p>Implements ReadOnlyNotification.
 *
 * @param listener  The journal
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void
notify_recovery_journal_of_read_only_mode(void *listener,
					  struct vdo_completion *parent)
{
	check_for_drain_complete(listener);
	complete_completion(parent);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All VIOs waiting for commits will be
 * awakened with an error.
 *
 * @param journal     The journal which has failed
 * @param error_code  The error result triggering this call
 **/
static void enter_journal_read_only_mode(struct recovery_journal *journal,
					 int error_code)
{
	enter_read_only_mode(journal->read_only_notifier, error_code);
	check_for_drain_complete(journal);
}

/**********************************************************************/
sequence_number_t
get_current_journal_sequence_number(struct recovery_journal *journal)
{
	return journal->tail;
}

/**
 * Get the head of the recovery journal, which is the lowest sequence number of
 * the block map head and the slab journal head.
 *
 * @param journal    The journal
 *
 * @return the head of the journal
 **/
static inline sequence_number_t
get_recovery_journal_head(struct recovery_journal *journal)
{
	return min_sequence_number(journal->block_map_head,
				   journal->slab_journal_head);
}

/**
 * Compute the recovery count byte for a given recovery count.
 *
 * @param recovery_count  The recovery count
 *
 * @return The byte corresponding to the recovery count
 **/
__attribute__((warn_unused_result)) static inline uint8_t
compute_recovery_count_byte(uint64_t recovery_count)
{
	return (uint8_t)(recovery_count & RECOVERY_COUNT_MASK);
}

/**
 * Check whether the journal is over the threshold, and if so, force the oldest
 * slab journal tail block to commit.
 *
 * @param journal    The journal
 **/
static void
check_slab_journal_commit_threshold(struct recovery_journal *journal)
{
	block_count_t current_length = journal->tail -
		journal->slab_journal_head;
	if (current_length > journal->slab_journal_commit_threshold) {
		journal->events.slab_journal_commits_requested++;
		commit_oldest_slab_journal_tail_blocks(journal->depot,
						       journal->slab_journal_head);
	}
}

/**********************************************************************/
static void reap_recovery_journal(struct recovery_journal *journal);
static void assign_entries(struct recovery_journal *journal);

/**
 * Finish reaping the journal.
 *
 * @param journal The journal being reaped
 **/
static void finish_reaping(struct recovery_journal *journal)
{
	sequence_number_t old_head = get_recovery_journal_head(journal);
	journal->block_map_head = journal->block_map_reap_head;
	journal->slab_journal_head = journal->slab_journal_reap_head;
	block_count_t blocks_reaped =
		get_recovery_journal_head(journal) - old_head;
	journal->available_space += blocks_reaped * journal->entries_per_block;
	journal->reaping = false;
	check_slab_journal_commit_threshold(journal);
	assign_entries(journal);
	check_for_drain_complete(journal);
}

/**
 * Finish reaping the journal after flushing the lower layer. This is the
 * callback registered in reap_recovery_journal().
 *
 * @param completion  The journal's flush VIO
 **/
static void complete_reaping(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;
	finish_reaping(journal);

	// Try reaping again in case more locks were released while flush was
	// out.
	reap_recovery_journal(journal);
}

/**
 * Handle an error when flushing the lower layer due to reaping.
 *
 * @param completion  The journal's flush VIO
 **/
static void handle_flush_error(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;
	journal->reaping = false;
	enter_journal_read_only_mode(journal, completion->result);
}

/**
 * Set all journal fields appropriately to start journaling from the current
 * active block.
 *
 * @param journal  The journal to be reset based on its active block
 **/
static void initialize_journal_state(struct recovery_journal *journal)
{
	journal->append_point.sequence_number = journal->tail;
	journal->last_write_acknowledged = journal->tail;
	journal->block_map_head = journal->tail;
	journal->slab_journal_head = journal->tail;
	journal->block_map_reap_head = journal->tail;
	journal->slab_journal_reap_head = journal->tail;
	journal->block_map_head_block_number =
		get_recovery_journal_block_number(journal,
						  journal->block_map_head);
	journal->slab_journal_head_block_number =
		get_recovery_journal_block_number(journal,
						  journal->slab_journal_head);
}

/**********************************************************************/
block_count_t get_recovery_journal_length(block_count_t journal_size)
{
	block_count_t reserved_blocks = journal_size / 4;
	if (reserved_blocks > RECOVERY_JOURNAL_RESERVED_BLOCKS) {
		reserved_blocks = RECOVERY_JOURNAL_RESERVED_BLOCKS;
	}
	return (journal_size - reserved_blocks);
}

/**
 * Attempt to reap the journal now that all the locks on some journal block
 * have been released. This is the callback registered with the lock counter.
 *
 * @param completion  The lock counter completion
 **/
static void reap_recovery_journal_callback(struct vdo_completion *completion)
{
	struct recovery_journal *journal =
		(struct recovery_journal *)completion->parent;
	// The acknowledgement must be done before reaping so that there is no
	// race between acknowledging the notification and unlocks wishing to
	// notify.
	acknowledge_unlock(journal->lock_counter);
	reap_recovery_journal(journal);
	check_slab_journal_commit_threshold(journal);
}

/**********************************************************************
 * Set the journal's tail sequence number.
 *
 * @param journal The journal whose tail is to be set
 * @param tail    The new tail value
 **/
static void set_journal_tail(struct recovery_journal *journal,
			     sequence_number_t tail)
{
	// VDO does not support sequence numbers above 1 << 48 in the slab
	// journal.
	if (tail >= (1ULL << 48)) {
		enter_journal_read_only_mode(journal, VDO_JOURNAL_OVERFLOW);
	}

	journal->tail = tail;
}

/**********************************************************************/
int make_recovery_journal(nonce_t nonce, PhysicalLayer *layer,
			  struct partition *partition,
			  uint64_t recovery_count,
			  block_count_t journal_size,
			  block_count_t tail_buffer_size,
			  struct read_only_notifier *read_only_notifier,
			  const struct thread_config *thread_config,
			  struct recovery_journal **journal_ptr)
{
	struct recovery_journal *journal;
	int result = ALLOCATE(1, struct recovery_journal, __func__, &journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initializeRing(&journal->free_tail_blocks);
	initializeRing(&journal->active_tail_blocks);
	initialize_wait_queue(&journal->pending_writes);

	journal->thread_id = get_journal_zone_thread(thread_config);
	journal->partition = partition;
	journal->nonce = nonce;
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	journal->size = journal_size;
	journal->read_only_notifier = read_only_notifier;
	journal->tail = 1;
	journal->slab_journal_commit_threshold = (journal_size * 2) / 3;
	initialize_journal_state(journal);

	journal->entries_per_block = RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
	block_count_t journal_length =
		get_recovery_journal_length(journal_size);
	journal->available_space = journal->entries_per_block * journal_length;

	// Only make the tail buffer and VIO in normal operation since the
	// formatter doesn't need them.
	if (layer->createMetadataVIO != NULL) {
		block_count_t i;
		for (i = 0; i < tail_buffer_size; i++) {
			struct recovery_journal_block *block;
			result = make_recovery_block(layer, journal, &block);
			if (result != VDO_SUCCESS) {
				free_recovery_journal(&journal);
				return result;
			}

			pushRingNode(&journal->free_tail_blocks,
				     &block->ring_node);
		}

		result = make_lock_counter(layer, journal,
					   reap_recovery_journal_callback,
					   journal->thread_id,
					   thread_config->logical_zone_count,
					   thread_config->physical_zone_count,
					   journal->size,
					   &journal->lock_counter);
		if (result != VDO_SUCCESS) {
			free_recovery_journal(&journal);
			return result;
		}

		result = ALLOCATE(VDO_BLOCK_SIZE, char, "journal flush data",
				  &journal->unused_flush_vio_data);
		if (result != VDO_SUCCESS) {
			free_recovery_journal(&journal);
			return result;
		}

		result = create_vio(layer, VIO_TYPE_RECOVERY_JOURNAL,
				    VIO_PRIORITY_HIGH, journal,
				    journal->unused_flush_vio_data,
				    &journal->flush_vio);
		if (result != VDO_SUCCESS) {
			free_recovery_journal(&journal);
			return result;
		}

		result = register_read_only_listener(read_only_notifier,
						     journal,
						     notify_recovery_journal_of_read_only_mode,
						     journal->thread_id);
		if (result != VDO_SUCCESS) {
			free_recovery_journal(&journal);
			return result;
		}

		journal->flush_vio->completion.callbackThreadID =
			journal->thread_id;
	}

	*journal_ptr = journal;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_recovery_journal(struct recovery_journal **journal_ptr)
{
	struct recovery_journal *journal = *journal_ptr;
	if (journal == NULL) {
		return;
	}

	free_lock_counter(&journal->lock_counter);
	free_vio(&journal->flush_vio);
	FREE(journal->unused_flush_vio_data);

	// XXX: eventually, the journal should be constructed in a quiescent
	// state
	//      which requires opening before use.
	if (!is_quiescent(&journal->state)) {
		ASSERT_LOG_ONLY(isRingEmpty(&journal->active_tail_blocks),
				"journal being freed has no active tail blocks");
	} else if (!is_saved(&journal->state)
		   && !isRingEmpty(&journal->active_tail_blocks)) {
		logWarning("journal being freed has uncommited entries");
	}

	struct recovery_journal_block *block;
	while ((block = pop_active_list(journal)) != NULL) {
		free_recovery_block(&block);
	}

	while ((block = pop_free_list(journal)) != NULL) {
		free_recovery_block(&block);
	}

	FREE(journal);
	*journal_ptr = NULL;
}

/**********************************************************************/
void set_recovery_journal_partition(struct recovery_journal *journal,
				    struct partition *partition)
{
	journal->partition = partition;
}

/**********************************************************************/
void initialize_recovery_journal_post_recovery(struct recovery_journal *journal,
					       uint64_t recovery_count,
					       sequence_number_t tail)
{
	set_journal_tail(journal, tail + 1);
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	initialize_journal_state(journal);
}

/**********************************************************************/
void
initialize_recovery_journal_post_rebuild(struct recovery_journal *journal,
					 uint64_t recovery_count,
					 sequence_number_t tail,
					 block_count_t logical_blocks_used,
					 block_count_t block_map_data_blocks)
{
	initialize_recovery_journal_post_recovery(journal, recovery_count,
						  tail);
	journal->logical_blocks_used = logical_blocks_used;
	journal->block_map_data_blocks = block_map_data_blocks;
}

/**********************************************************************/
block_count_t
get_journal_block_map_data_blocks_used(struct recovery_journal *journal)
{
	return journal->block_map_data_blocks;
}

/**********************************************************************/
void set_journal_block_map_data_blocks_used(struct recovery_journal *journal,
					    block_count_t pages)
{
	journal->block_map_data_blocks = pages;
}

/**********************************************************************/
thread_id_t get_recovery_journal_thread_id(struct recovery_journal *journal)
{
	return journal->thread_id;
}

/**********************************************************************/
void open_recovery_journal(struct recovery_journal *journal,
			   struct slab_depot *depot,
			   struct block_map *block_map)
{
	journal->depot = depot;
	journal->block_map = block_map;
	journal->state.state = ADMIN_STATE_NORMAL_OPERATION;
}

/**********************************************************************/
size_t get_recovery_journal_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0);
}

/**********************************************************************/
int encode_recovery_journal(struct recovery_journal *journal,
			    struct buffer *buffer)
{
	sequence_number_t journal_start;
	if (is_saved(&journal->state)) {
		// If the journal is saved, we should start one past the active
		// block (since the active block is not guaranteed to be empty).
		journal_start = journal->tail;
	} else {
		// When we're merely suspended or have gone read-only, we must
		// record the first block that might have entries that need to
		// be applied.
		journal_start = get_recovery_journal_head(journal);
	}

	int result = encode_header(&RECOVERY_JOURNAL_HEADER_7_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = put_uint64_le_into_buffer(buffer, journal_start);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, journal->logical_blocks_used);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, journal->block_map_data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == encoded_size,
		      "encoded recovery journal component size must match header size");
}

/**
 * Decode recovery journal component state version 7.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
decodeRecoveryJournalState_7_0(struct buffer *buffer,
			       struct recovery_journal_state_7_0 *state)
{
	size_t initial_length = content_length(buffer);

	sequence_number_t journal_start;
	int result = get_uint64_le_from_buffer(buffer, &journal_start);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t logical_blocks_used;
	result = get_uint64_le_from_buffer(buffer, &logical_blocks_used);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t block_map_data_blocks;
	result = get_uint64_le_from_buffer(buffer, &block_map_data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*state = (struct recovery_journal_state_7_0) {
		.journal_start = journal_start,
		.logical_blocks_used = logical_blocks_used,
		.block_map_data_blocks = block_map_data_blocks,
	};

	size_t decoded_size = initial_length - content_length(buffer);
	return ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == decoded_size,
		      "decoded slab depot component size must match header size");
}

/**********************************************************************/
int decode_recovery_journal(struct recovery_journal *journal,
			    struct buffer *buffer)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&RECOVERY_JOURNAL_HEADER_7_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct recovery_journal_state_7_0 state;
	result = decodeRecoveryJournalState_7_0(buffer, &state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Update recovery journal in-memory information.
	set_journal_tail(journal, state.journal_start);
	journal->logical_blocks_used = state.logical_blocks_used;
	journal->block_map_data_blocks = state.block_map_data_blocks;
	initialize_journal_state(journal);

	// XXX: this is a hack until we make initial resume of a VDO a real
	// resume
	journal->state.state = ADMIN_STATE_SUSPENDED;
	return VDO_SUCCESS;
}

/**********************************************************************/
int decode_sodium_recovery_journal(struct recovery_journal *journal,
				   struct buffer *buffer)
{
	// Sodium uses version 7.0, same as head, currently.
	return decode_recovery_journal(journal, buffer);
}

/**
 * Advance the tail of the journal.
 *
 * @param journal  The journal whose tail should be advanced
 *
 * @return <code>true</code> if the tail was advanced
 **/
static bool advance_tail(struct recovery_journal *journal)
{
	journal->active_block = pop_free_list(journal);
	if (journal->active_block == NULL) {
		return false;
	}

	pushRingNode(&journal->active_tail_blocks,
		     &journal->active_block->ring_node);
	initialize_recovery_block(journal->active_block);
	set_journal_tail(journal, journal->tail + 1);
	advance_block_map_era(journal->block_map, journal->tail);
	return true;
}

/**
 * Check whether there is space to make a given type of entry.
 *
 * @param journal    The journal to check
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to make an
 *         entry of the specified type
 **/
static bool check_for_entry_space(struct recovery_journal *journal,
				  bool increment)
{
	if (increment) {
		return ((journal->available_space
			 - journal->pending_decrement_count)
			> 1);
	}

	return (journal->available_space > 0);
}

/**
 * Prepare the currently active block to receive an entry and check whether
 * an entry of the given type may be assigned at this time.
 *
 * @param journal    The journal receiving an entry
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to store an
 *         entry of the specified type
 **/
static bool prepare_to_assign_entry(struct recovery_journal *journal,
				    bool increment)
{
	if (!check_for_entry_space(journal, increment)) {
		if (!increment) {
			// There must always be room to make a decrement entry.
			logError("No space for decrement entry in recovery journal");
			enter_journal_read_only_mode(journal,
						     VDO_RECOVERY_JOURNAL_FULL);
		}
		return false;
	}

	if (is_recovery_block_full(journal->active_block)
	    && !advance_tail(journal)) {
		return false;
	}

	if (!is_recovery_block_empty(journal->active_block)) {
		return true;
	}

	if ((journal->tail - get_recovery_journal_head(journal)) >
	    journal->size) {
		// Cannot use this block since the journal is full.
		journal->events.disk_full++;
		return false;
	}

	/*
	 * Don't allow the new block to be reaped until all of its entries have
	 * been committed to the block map and until the journal block has been
	 * fully committed as well. Because the block map update is done only
	 * after any slab journal entries have been made, the per-entry lock for
	 * the block map entry serves to protect those as well.
	 */
	initialize_lock_count(journal->lock_counter,
			      journal->active_block->block_number,
			      journal->entries_per_block + 1);
	return true;
}

static void write_blocks(struct recovery_journal *journal);

/**
 * Queue a block for writing. The block is expected to be full. If the block
 * is currently writing, this is a noop as the block will be queued for
 * writing when the write finishes. The block must not currently be queued
 * for writing.
 *
 * @param journal  The journal in question
 * @param block    The block which is now ready to write
 **/
static void schedule_block_write(struct recovery_journal *journal,
				 struct recovery_journal_block *block)
{
	if (block->committing) {
		return;
	}

	int result = enqueue_waiter(&journal->pending_writes,
				    &block->write_waiter);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		return;
	}

	PhysicalLayer *layer = vio_as_completion(journal->flush_vio)->layer;
	if ((layer->getWritePolicy(layer) == WRITE_POLICY_ASYNC)) {
		/*
		 * At the end of adding entries, or discovering this partial
		 * block is now full and ready to rewrite, we will call
		 * write_blocks() and write a whole batch.
		 */
		return;
	}
	write_blocks(journal);
}

/**
 * Release a reference to a journal block.
 *
 * @param block  The journal block from which to release a reference
 **/
static void release_journal_block_reference(struct recovery_journal_block *block)
{
	release_journal_zone_reference(block->journal->lock_counter,
				       block->block_number);
}

/**
 * Implements waiter_callback. Assign an entry waiter to the active block.
 **/
static void assign_entry(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal_block *block =
		(struct recovery_journal_block *)context;
	struct recovery_journal *journal = block->journal;

	// Record the point at which we will make the journal entry.
	data_vio->recoveryJournalPoint = (struct journal_point) {
		.sequence_number = block->sequence_number,
		.entry_count = block->entry_count,
	};

	switch (data_vio->operation.type) {
	case DATA_INCREMENT:
		if (data_vio->operation.state != MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used++;
		}
		journal->pending_decrement_count++;
		break;

	case DATA_DECREMENT:
		if (data_vio->operation.state != MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used--;
		}

		// Per-entry locks need not be held for decrement entries since
		// the lock held for the incref entry will protect this entry as
		// well.
		release_journal_block_reference(block);
		ASSERT_LOG_ONLY((journal->pending_decrement_count != 0),
				"decrement follows increment");
		journal->pending_decrement_count--;
		break;

	case BLOCK_MAP_INCREMENT:
		journal->block_map_data_blocks++;
		break;

	default:
		logError("Invalid journal operation %u",
			 data_vio->operation.type);
		enter_journal_read_only_mode(journal, VDO_NOT_IMPLEMENTED);
		continue_data_vio(data_vio, VDO_NOT_IMPLEMENTED);
		return;
	}

	journal->available_space--;
	int result = enqueue_recovery_block_entry(block, data_vio);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		continue_data_vio(data_vio, result);
	}

	if (is_recovery_block_full(block)) {
		// The block is full, so we can write it anytime henceforth. If
		// it is already committing, we'll queue it for writing when it
		// comes back.
		schedule_block_write(journal, block);
	}

	// Force out slab journal tail blocks when threshold is reached.
	check_slab_journal_commit_threshold(journal);
}

/**********************************************************************/
static bool assign_entries_from_queue(struct recovery_journal *journal,
				      struct wait_queue *queue, bool increment)
{
	while (has_waiters(queue)) {
		if (!prepare_to_assign_entry(journal, increment)) {
			return false;
		}

		notify_next_waiter(queue, assign_entry, journal->active_block);
	}

	return true;
}

/**********************************************************************/
static void assign_entries(struct recovery_journal *journal)
{
	if (journal->adding_entries) {
		// Protect against re-entrancy.
		return;
	}

	journal->adding_entries = true;
	if (assign_entries_from_queue(journal, &journal->decrement_waiters,
				      false)) {
		assign_entries_from_queue(journal, &journal->increment_waiters,
					  true);
	}

	// Now that we've finished with entries, see if we have a batch of
	// blocks to write.
	write_blocks(journal);
	journal->adding_entries = false;
}

/**
 * Prepare an in-memory journal block to be reused now that it has been fully
 * committed.
 *
 * @param block  The block to be recycled
 **/
static void recycle_journal_block(struct recovery_journal_block *block)
{
	struct recovery_journal *journal = block->journal;
	pushRingNode(&journal->free_tail_blocks, &block->ring_node);

	// Release any unused entry locks.
	block_count_t i;
	for (i = block->entry_count; i < journal->entries_per_block; i++) {
		release_journal_block_reference(block);
	}

	// Release our own lock against reaping now that the block is completely
	// committed, or we're giving up because we're in read-only mode.
	if (block->entry_count > 0) {
		release_journal_block_reference(block);
	}

	if (block == journal->active_block) {
		journal->active_block = NULL;
	}
}

/**
 * waiter_callback implementation invoked whenever a VIO is to be released
 * from the journal because its entry was committed to disk.
 **/
static void continue_committed_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal *journal = (struct recovery_journal *)context;
	ASSERT_LOG_ONLY(before_journal_point(&journal->commit_point,
					     &data_vio->recoveryJournalPoint),
			"DataVIOs released from recovery journal in order. "
			"Recovery journal point is (%llu, %" PRIu16
			"), "
			"but commit waiter point is (%llu, %" PRIu16 ")",
			journal->commit_point.sequence_number,
			journal->commit_point.entry_count,
			data_vio->recoveryJournalPoint.sequence_number,
			data_vio->recoveryJournalPoint.entry_count);
	journal->commit_point = data_vio->recoveryJournalPoint;

	int result = (is_read_only(journal->read_only_notifier) ? VDO_READ_ONLY
							      : VDO_SUCCESS);
	continue_waiter(waiter, &result);
}

/**
 * Notify any VIOs whose entries have now committed, and recycle any
 * journal blocks which have been fully committed.
 *
 * @param journal  The recovery journal to update
 **/
static void notify_commit_waiters(struct recovery_journal *journal)
{
	struct recovery_journal_block *last_iteration_block = NULL;
	while (!isRingEmpty(&journal->active_tail_blocks)) {
		struct recovery_journal_block *block =
			block_from_ring_node(journal->active_tail_blocks.next);

		int result = ASSERT(block != last_iteration_block,
				    "Journal notification has entered an infinite loop");
		if (result != VDO_SUCCESS) {
			enter_journal_read_only_mode(journal, result);
			return;
		}
		last_iteration_block = block;

		if (block->committing) {
			return;
		}

		notify_all_waiters(&block->commit_waiters,
				   continue_committed_waiter, journal);
		if (is_read_only(journal->read_only_notifier)) {
			notify_all_waiters(&block->entry_waiters,
					   continue_committed_waiter, journal);
		} else if (is_recovery_block_dirty(block)
			   || !is_recovery_block_full(block)) {
			// Don't recycle partially-committed or partially-filled
			// blocks.
			return;
		}

		recycle_journal_block(block);
	}
}

/**
 * Handle post-commit processing. This is the callback registered by
 * write_block(). If more entries accumulated in the block being committed while
 * the commit was in progress, another commit will be initiated.
 *
 * @param completion  The completion of the VIO writing this block
 **/
static void complete_write(struct vdo_completion *completion)
{
	struct recovery_journal_block *block = completion->parent;
	struct recovery_journal *journal = block->journal;
	assert_on_journal_thread(journal, __func__);

	journal->pending_write_count -= 1;
	journal->events.blocks.committed += 1;
	journal->events.entries.committed += block->entries_in_commit;
	block->uncommitted_entry_count -= block->entries_in_commit;
	block->entries_in_commit = 0;
	block->committing = false;

	// If this block is the latest block to be acknowledged, record that
	// fact.
	if (block->sequence_number > journal->last_write_acknowledged) {
		journal->last_write_acknowledged = block->sequence_number;
	}

	struct recovery_journal_block *last_active_block =
		block_from_ring_node(journal->active_tail_blocks.next);
	ASSERT_LOG_ONLY((block->sequence_number >=
			 last_active_block->sequence_number),
			"completed journal write is still active");

	notify_commit_waiters(journal);

	// Is this block now full? Reaping, and adding entries, might have
	// already sent it off for rewriting; else, queue it for rewrite.
	if (is_recovery_block_dirty(block) && is_recovery_block_full(block)) {
		schedule_block_write(journal, block);
	}

	write_blocks(journal);

	check_for_drain_complete(journal);
}

/**********************************************************************/
static void handle_write_error(struct vdo_completion *completion)
{
	struct recovery_journal_block *block = completion->parent;
	struct recovery_journal *journal = block->journal;
	logErrorWithStringError(completion->result,
				"cannot write recovery journal block %llu",
				block->sequence_number);
	enter_journal_read_only_mode(journal, completion->result);
	complete_write(completion);
}

/**
 * Issue a block for writing. Implements waiter_callback.
 **/
static void write_block(struct waiter *waiter,
			void *context __attribute__((unused)))
{
	struct recovery_journal_block *block = block_from_waiter(waiter);
	if (is_read_only(block->journal->read_only_notifier)) {
		return;
	}

	int result = commit_recovery_block(block,
					   complete_write,
					   handle_write_error);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(block->journal, result);
	}
}

/**
 * Attempt to commit blocks, according to write policy.
 *
 * @param journal     The recovery journal
 **/
static void write_blocks(struct recovery_journal *journal)
{
	assert_on_journal_thread(journal, __func__);
	/*
	 * In sync and async-unsafe modes, we call this function each time we
	 * queue a full block on pending writes; in addition, in all cases we
	 * call this function after adding entries to the journal and finishing
	 * a block write. Thus, when this function terminates we must either
	 * have no VIOs waiting in the journal or have some outstanding IO to
	 * provide a future wakeup.
	 *
	 * In all modes, if there are no outstanding writes and some unwritten
	 * entries, we must issue a block, even if it's the active block and it
	 * isn't full. Otherwise, in sync/async-unsafe modes, we want to issue
	 * all full blocks every time; since we call it each time we fill a
	 * block, this is equivalent to issuing every full block as soon as its
	 * full. In async mode, we want to only issue full blocks if there are
	 * no pending writes.
	 */

	PhysicalLayer *layer = vio_as_completion(journal->flush_vio)->layer;
	if ((layer->getWritePolicy(layer) != WRITE_POLICY_ASYNC)
	    || (journal->pending_write_count == 0)) {
		// Write all the full blocks.
		notify_all_waiters(&journal->pending_writes, write_block, NULL);
	}

	// Do we need to write the active block? Only if we have no outstanding
	// writes, even after issuing all of the full writes.
	if ((journal->pending_write_count == 0)
	    && can_commit_recovery_block(journal->active_block)) {
		write_block(&journal->active_block->write_waiter, NULL);
	}
}

/**********************************************************************/
void add_recovery_journal_entry(struct recovery_journal *journal,
				struct data_vio *data_vio)
{
	assert_on_journal_thread(journal, __func__);
	if (!is_normal(&journal->state)) {
		continue_data_vio(data_vio, VDO_INVALID_ADMIN_STATE);
		return;
	}

	if (is_read_only(journal->read_only_notifier)) {
		continue_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	bool increment = is_increment_operation(data_vio->operation.type);
	ASSERT_LOG_ONLY((!increment || (data_vio->recoverySequenceNumber == 0)),
			"journal lock not held for increment");

	advance_journal_point(&journal->append_point, journal->entries_per_block);
	int result = enqueue_data_vio((increment ? &journal->increment_waiters
				     	: &journal->decrement_waiters),
				      data_vio,
				      THIS_LOCATION("$F($j-$js);io=journal($j-$js)"));
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		continue_data_vio(data_vio, result);
		return;
	}

	assign_entries(journal);
}

/**
 * Conduct a sweep on a recovery journal to reclaim unreferenced blocks.
 *
 * @param journal  The recovery journal
 **/
static void reap_recovery_journal(struct recovery_journal *journal)
{
	if (journal->reaping) {
		// We already have an outstanding reap in progress. We need to
		// wait for it to finish.
		return;
	}

	// Start reclaiming blocks only when the journal head has no references.
	// Then stop when a block is referenced.
	while ((journal->block_map_reap_head < journal->last_write_acknowledged)
	       && !is_locked(journal->lock_counter,
			     journal->block_map_head_block_number,
			     ZONE_TYPE_LOGICAL)) {
		journal->block_map_reap_head++;
		if (++journal->block_map_head_block_number == journal->size) {
			journal->block_map_head_block_number = 0;
		}
	}

	while ((journal->slab_journal_reap_head < journal->last_write_acknowledged)
	       && !is_locked(journal->lock_counter,
			     journal->slab_journal_head_block_number,
			     ZONE_TYPE_PHYSICAL)) {
		journal->slab_journal_reap_head++;
		if (++journal->slab_journal_head_block_number == journal->size) {
			journal->slab_journal_head_block_number = 0;
		}
	}

	if ((journal->block_map_reap_head == journal->block_map_head)
	    && (journal->slab_journal_reap_head == journal->slab_journal_head)) {
		// Nothing happened.
		return;
	}

	PhysicalLayer *layer = vio_as_completion(journal->flush_vio)->layer;
	if (layer->getWritePolicy(layer) != WRITE_POLICY_SYNC) {
		/*
		 * If the block map head will advance, we must flush any block
		 * map page modified by the entries we are reaping. If the slab
		 * journal head will advance, we must flush the slab summary
		 * update covering the slab journal that just released some
		 * lock.
		 *
		 * In sync mode, this is unnecessary because we won't record
		 * these numbers on disk until the next journal block write,
		 * and in sync mode every journal block write is preceded by
		 * a flush, which does the block map page and slab summary
		 * update flushing itself.
		 */
		journal->reaping = true;
		launch_flush(journal->flush_vio, complete_reaping,
			     handle_flush_error);
		return;
	}

	finish_reaping(journal);
}

/**********************************************************************/
void acquire_recovery_journal_block_reference(struct recovery_journal *journal,
					      sequence_number_t sequence_number,
					      zone_type zone_type,
					      zone_count_t zone_id)
{
	if (sequence_number == 0) {
		return;
	}

	block_count_t block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	acquire_lock_count_reference(journal->lock_counter, block_number,
				     zone_type, zone_id);
}

/**********************************************************************/
void release_recovery_journal_block_reference(struct recovery_journal *journal,
					      sequence_number_t sequence_number,
					      zone_type zone_type,
					      zone_count_t zone_id)
{
	if (sequence_number == 0) {
		return;
	}

	block_count_t block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	release_lock_count_reference(journal->lock_counter, block_number,
				     zone_type, zone_id);
}

/**********************************************************************/
void release_per_entry_lock_from_other_zone(struct recovery_journal *journal,
					    sequence_number_t sequence_number)
{
	if (sequence_number == 0) {
		return;
	}

	block_count_t block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	release_journal_zone_reference_from_other_zone(journal->lock_counter,
						       block_number);
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct recovery_journal,
					      state));
}

/**********************************************************************/
void drain_recovery_journal(struct recovery_journal *journal,
			    AdminStateCode operation,
			    struct vdo_completion *parent)
{
	assert_on_journal_thread(journal, __func__);
	start_draining(&journal->state, operation, parent, initiate_drain);
}

/**********************************************************************/
void resume_recovery_journal(struct recovery_journal *journal,
			     struct vdo_completion *parent)
{
	assert_on_journal_thread(journal, __func__);
	bool saved = is_saved(&journal->state);
	set_completion_result(parent, resume_if_quiescent(&journal->state));

	if (is_read_only(journal->read_only_notifier)) {
		finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	if (saved) {
		initialize_journal_state(journal);
	}

	complete_completion(parent);
}

/**********************************************************************/
block_count_t
get_journal_logical_blocks_used(const struct recovery_journal *journal)
{
	return journal->logical_blocks_used;
}

/**********************************************************************/
struct recovery_journal_statistics
get_recovery_journal_statistics(const struct recovery_journal *journal)
{
	return journal->events;
}

/**********************************************************************/
void dump_recovery_journal_statistics(const struct recovery_journal *journal)
{
	struct recovery_journal_statistics stats =
		get_recovery_journal_statistics(journal);
	logInfo("Recovery Journal");
	logInfo("  block_map_head=%llu slab_journal_head=%" PRIu64
		" last_write_acknowledged=%llu tail=%" PRIu64
		" block_map_reap_head=%" PRIu64
		" slab_journal_reap_head=%" PRIu64
		" diskFull=%llu slabJournalCommitsRequested=%" PRIu64
		" increment_waiters=%zu decrement_waiters=%zu",
		journal->block_map_head, journal->slab_journal_head,
		journal->last_write_acknowledged, journal->tail,
		journal->block_map_reap_head, journal->slab_journal_reap_head,
		stats.disk_full, stats.slab_journal_commits_requested,
		count_waiters(&journal->increment_waiters),
		count_waiters(&journal->decrement_waiters));
	logInfo("  entries: started=%llu written=%" PRIu64
		" committed=%llu",
		stats.entries.started, stats.entries.written,
		stats.entries.committed);
	logInfo("  blocks: started=%llu written=%" PRIu64
		" committed=%llu",
		stats.blocks.started, stats.blocks.written,
		stats.blocks.committed);

	logInfo("  active blocks:");
	const RingNode *head = &journal->active_tail_blocks;
	RingNode *node;
	for (node = head->next; node != head; node = node->next) {
		dump_recovery_block(block_from_ring_node(node));
	}
}
