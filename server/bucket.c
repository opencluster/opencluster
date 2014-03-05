// bucket.c


// By setting __BUCKET_C, we indicate that we dont want the externs to be defined.
#define __BUCKET_C
#include "bucket.h"
#undef __BUCKET_C

#include "globals.h"
#include "item.h"
#include "logging.h"
#include "push.h"
#include "server.h"
#include "stats.h"
#include "timeout.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// the mask is used to determine which bucket a hash belongs to.
hash_t _mask = 0;


// the list of buckets that this server is handling.  '_mask' indicates how many entries in the 
// array there is, but the ones that are not handled by this server, will have a NULL entry.
bucket_t ** _buckets = NULL;


// keep a count of the number of buckets we have that do not have backup copies.  This is used to 
// speed up certain operations to avoid having to iterate through the list of buckets to find out 
// if we have any that are backup-less.   So it is important to keep this value accurate or 
// certain importain options might get skipped incorrectly.
int _nobackup_buckets = 0;


// keep a count of the number of primary and secondary buckets this node has.
int _primary_buckets = 0;
int _secondary_buckets = 0;

// 0 if no buckets are currently being transferred, 1 if there is.  There can only be one transfer 
// at a time.
int _bucket_transfer = 0;

// the list of hashmasks is to know which servers are responsible for which buckets.
// this list of hashmasks will also replace the need to 'settle'.  Instead, a timed event will 
// occur periodically that checks that the hashmask coverage is complete.
hashmask_t ** _hashmasks = NULL;


// When a migration of a bucket needs to occur, and only one migration can occur at a time, we need 
// to send ALL the data for this bucket to the other node, we need to have a way to tell if an item 
// has already been transferred, and if it hasn't.   And we dont particularly want to go through the 
// list first marking a flag.  So instead, each hash item will have an integer, and we will have a 
// master integer (_migrate_sync).  When we increment the master integer, which will immediately 
// make the integer in all the items obsolete.   Now the tree can be searched for items that have an 
// outdated integer, transfer that item, and update the integer to match the master integer.   Since 
// it will need to look at the chain of trees, this process will also work for them also.   Any 
// items founds in the sub-chains will need to be moved to the current chain as it continues this 
// process.
int _migrate_sync = 0;



// get a value from whichever bucket is resposible.
value_t * buckets_get_value(hash_t map_hash, hash_t key_hash) 
{
	int bucket_index;
	bucket_t *bucket;
	value_t *value = NULL;

	// calculate the bucket that this item belongs in.
	bucket_index = _mask & key_hash;
	assert(bucket_index >= 0);
	assert(bucket_index <= _mask);
	bucket = _buckets[bucket_index];

	// if we have a record for this bucket, then we are either a primary or a backup for it.
	if (bucket) {

		assert(bucket->hash == bucket_index);
		
		// make sure that this server is 'primary' for this bucket.
		if (bucket->level != 0) {
			// we need to reply with an indication of which server is actually responsible for this bucket.
			assert(value == NULL);
		}
		else {
			// search the btree in the bucket for this key.
			assert(bucket->data);
			value = data_get_value(map_hash, key_hash, bucket->data);
		}	
	}
	else {
		assert(value == NULL);
	}
	
	return(value);
}



// store the value in whatever bucket is resposible for the key_hash.
// NOTE: value is controlled by the tree after this function call.
// NOTE: name is controlled by the tree after this function call.
int buckets_store_value(hash_t map_hash, hash_t key_hash, char *name, long long name_int, int expires, value_t *value) 
{
	int bucket_index;
	bucket_t *bucket;
	client_t *backup_client;

	// calculate the bucket that this item belongs in.
	bucket_index = _mask & key_hash;
	assert(bucket_index >= 0);
	assert(bucket_index <= _mask);
	bucket = _buckets[bucket_index];

	// if we have a record for this bucket, then we are (potentially) either a primary or a backup 
	// for it.
	if (bucket) {
		assert(bucket->hash == bucket_index);
		if (bucket->backup_node) {
			// since we have a backup_node specified, then we must be the primary.
			backup_client = bucket->backup_node->client;
			assert(backup_client);
		}
		else {
			// since we dont have a backup_node specified, then we are the backup and dont need to 
			// send the data anywhere else.
			backup_client = NULL;
		}
		
		data_set_value(map_hash, key_hash, bucket->data, name, name_int, value, expires, backup_client);
		return(0);
	}
	else {
		assert(0);
		return(-1);
	}
}



bucket_t * bucket_new(hash_t hash)
{
	bucket_t *bucket;
	
	assert(_buckets[hash] == NULL);
	
	bucket = calloc(1, sizeof(bucket_t));
	bucket->hash = hash;
	bucket->level = -1;
			
	assert(bucket->backup_node == NULL);
	assert(bucket->target_node == NULL);
	assert(bucket->logging_node == NULL);
	assert(bucket->transfer_event == NULL);
	assert(bucket->shutdown_event == NULL);
	assert(bucket->transfer_client == NULL);
	assert(data_in_transit() == 0);
	assert(bucket->oldbucket_event == NULL);
	assert(bucket->transfer_mode_special == 0);
	assert(bucket->promoting == NOT_PROMOTING);
			
	bucket->data = data_new(hash);
	
	return(bucket);
}



// delete the contents of the bucket.  Note, that the bucket becomes empty, but the bucket itself is 
// not destroyed.
void bucket_destroy_contents(bucket_t *bucket)
{
	assert(bucket);
	
	// at this point, since the bucket is being destroyed, there should be a connected transfer client.
	assert(bucket->transfer_client == NULL);

	if (bucket->data) {
		data_destroy(bucket->data, bucket->hash);
		data_free(bucket->data);
		bucket->data = NULL;
	}
	
	assert(bucket->data == NULL);
}


static void bucket_close(bucket_t *bucket)
{
	assert(bucket);
	
	
	// cant remember what this function is used for, need to look through the code that calls it.
	// it might be the same as bucket_destroy.
	assert(0);
}




// this function will take the current array, and put it aside, creating a new array based on the 
// new mask supplied (we can only make the mask bigger, and cannot shrink it).
// We create a new hashmasks array, and for each entry, we compare it against the old mask, and use 
// the data for that hash from the old list.
// NOTE: We may be starting off with an empty hashmasks lists.  If this is the first time we've 
//       received some hashmasks.  To implement this easily, if we dont already have hashmasks, we 
//       may need to create one that has a dummy entry in it.
void buckets_split_mask(hash_t mask) 
{
	hashmask_t **newlist = NULL;
	hashmask_t **oldlist = NULL;
	
	bucket_t **newbuckets = NULL;
	bucket_t **oldbuckets = NULL;
	
	int i;
	int index;
	
	assert(mask > _mask);
	
	logger(LOG_INFO, "Splitting bucket list: oldmask=%#llx, newmask=%#llx", _mask, mask);
	
	// first grab a copy of the existing hashmasks as the 'oldlist'
	oldlist = _hashmasks;
	_hashmasks = NULL;
	if (oldlist == NULL) {
		
		oldlist = malloc(sizeof(hashmask_t *));
		assert(oldlist);
		
		// need to create at least one dummy entry so that we can split it to the new entries.
		oldlist[0] = malloc(sizeof(hashmask_t));
		assert(oldlist[0]);
		
		oldlist[0]->primary = NULL;
		oldlist[0]->secondary = NULL;
	}
	
	// grab a copy of the existing buckets as the 'oldbuckets';
	oldbuckets = _buckets;
	_buckets = NULL;
	
	// make an appropriate sized new hashmask list.
	newlist = malloc(sizeof(hashmask_t *) * (mask+1));
	assert(newlist);
	
	// make an appropriate sized new buckets list;
	newbuckets = malloc(sizeof(bucket_t *) * (mask+1));
	assert(newbuckets);
	
	// go through every hash for this mask.
	for (i=0; i<=mask; i++) {

		// determine what the old index is.
		index = i & _mask;
		
		// create the new hashmask entry for the new index.  Copy the strings from the old index.
		newlist[i] = malloc(sizeof(hashmask_t));
		assert(_mask == 0 || index < _mask);
		if (oldlist[index]->primary) { newlist[i]->primary = strdup(oldlist[index]->primary); }
		else { newlist[i]->primary = NULL; }
		if (oldlist[index]->secondary) { newlist[i]->secondary = strdup(oldlist[index]->secondary); }
		else { newlist[i]->primary = NULL; }
		
		// create the new bucket ONLY if we already have a bucket object for that index.
		if (oldbuckets == NULL || oldbuckets[index] == NULL) {
			newbuckets[i] = NULL;
		}
		else {
			// we have a bucket for this old index.  So we need to create a new one for this index.
			
			
			newbuckets[i] = bucket_new(i);

			assert(newbuckets[i]->data);
			assert(newbuckets[i]->data->next == NULL);
			assert(oldbuckets[index]->data);
			assert(oldbuckets[index]->data->ref > 0);
			newbuckets[i]->data->next = oldbuckets[index]->data;
			oldbuckets[index]->data->ref ++;

			assert(newbuckets[i]->hash == i);
			newbuckets[i]->level = oldbuckets[index]->level;
			
			newbuckets[i]->target_node = oldbuckets[index]->target_node;
			newbuckets[i]->backup_node = oldbuckets[index]->backup_node;
			newbuckets[i]->logging_node = oldbuckets[index]->logging_node;
			
			assert(data_in_transit() == 0);
		}
	}

	
	// ---------
	
	
	// now we clean up the old hashmask list.
	for (i=0; i<=_mask; i++) {
		assert(oldlist[i]);
		if (oldlist[i]->primary) {
			free(oldlist[i]->primary);
		}
		if (oldlist[i]->secondary) {
			free(oldlist[i]->secondary);
		}
		free(oldlist[i]);
	}
	free(oldlist);
	oldlist = NULL;
	
	
	// clean up the old buckets list.
	if (oldbuckets) {
		for (i=0; i<=_mask; i++) {
			if (oldbuckets[i]) {
				assert(oldbuckets[i]->data);
				assert(oldbuckets[i]->data->ref > 1);
				oldbuckets[i]->data->ref --;
				assert(oldbuckets[i]->data->ref > 0);
				
				oldbuckets[i]->data = NULL;
				
				bucket_close(oldbuckets[i]);
			}
		}
	}	
	
	_hashmasks = (void *) newlist;
	_buckets = newbuckets;
	_mask = mask;
	
	
	assert(_mask > 0);
	assert(_hashmasks);
	assert(_buckets);
}


int buckets_nobackup_count(void)
{
	assert(_nobackup_buckets >= 0);
	return(_nobackup_buckets);
}


// check the integrity of the empty bucket, and then free the memory it uses.
static void bucket_free(bucket_t *bucket)
{
	assert(bucket);
	assert(bucket->level < 0);
	assert(bucket->data == NULL);
	assert(bucket->target_node == NULL);
	assert(bucket->backup_node == NULL);
	assert(bucket->logging_node == NULL);
	assert(bucket->transfer_client == NULL);
	assert(bucket->transfer_mode_special == 0);
	assert(bucket->shutdown_event == NULL);
	assert(bucket->transfer_event == NULL);
	assert(bucket->oldbucket_event == NULL);
	
	free(bucket);
}



static void bucket_shutdown_handler(evutil_socket_t fd, short what, void *arg) 
{
	bucket_t *bucket = arg;
	int done = 0;
	
	assert(fd == -1);
	assert(arg);
	assert(bucket);
	assert(bucket->shutdown_event);
	
	// if the bucket is a backup bucket, we can simply destroy it, and send out a message to clients 
	// that it is no longer the backup for the bucket.
	if (bucket->level > 0) {
		done ++;
	}
	else {
		assert(bucket->level == 0);

		// if the bucket is primary, but there are no nodes to send it to, then we destroy it.
		if (_node_count == 0) {
			done ++;
		}
		else {
			assert(_node_count > 0);
			
			// If the backup node is connected, then we will tell that node, that it has been 
			// promoted to be primary for the bucket.
			if (bucket->backup_node) {
				assert(bucket->backup_node->client);
				push_promote(bucket->backup_node->client, bucket->hash);

				assert(bucket->promoting == NOT_PROMOTING);
				bucket->promoting = PROMOTING;
				
				done ++;
			}
			else {
				// at this point, we are the primary and there is no backup.  There are other nodes 
				// connected, so we need to try and transfer this bucket to another node.
				assert(0);
				
				assert(done == 0);
				
// 				assert(_buckets[i]->transfer_event == NULL);
// 				_buckets[i]->transfer_event = evtimer_new(_evbase, bucket_transfer_handler, _buckets[i]);
// 				assert(_buckets[i]->transfer_event);
			}
		}
	}
	
	if (done > 0) {
		// we are done with the bucket.
	
		assert(bucket->transfer_client == NULL);
		bucket_destroy_contents(bucket);
		push_hashmask_update(bucket);
				
		assert(bucket->shutdown_event);
		event_free(bucket->shutdown_event);
		bucket->shutdown_event = NULL;

		assert(_buckets[bucket->hash] == bucket);
		_buckets[bucket->hash] = NULL;
		
		bucket_free(bucket);
		bucket = NULL;
	}
	else {
		// we are not done yet, so we need to schedule the event again.
		assert(bucket->shutdown_event);
		evtimer_add(bucket->shutdown_event, &_timeout_shutdown);
	}		
}



// if the shutdown process has not already been started, then we need to start it.  Otherwise do nothing.
void bucket_shutdown(bucket_t *bucket)
{
	assert(bucket);
	
	if (bucket->shutdown_event == NULL) {
		printf("Bucket shutdown initiated: %#llx\n", bucket->hash);

		assert(_evbase);
		bucket->shutdown_event = evtimer_new(_evbase, bucket_shutdown_handler, bucket);
		assert(bucket->shutdown_event);
		evtimer_add(bucket->shutdown_event, &_timeout_now);
	}
}



void buckets_init(void)
{
	int i;
	
	assert(_mask > 0);
	
	_buckets = calloc(_mask+1, sizeof(bucket_t *));
	assert(_buckets);

	assert(_primary_buckets == 0);
	assert(_secondary_buckets == 0);
	
	assert(_hashmasks == NULL);
	_hashmasks = calloc(_mask+1, sizeof(hashmask_t *));
	assert(_hashmasks);

	// for starters we will need to create a bucket for each hash.
	for (i=0; i<=_mask; i++) {
		_buckets[i] = bucket_new(i);

		_primary_buckets ++;
		_buckets[i]->level = 0;

		// send out a message to all connected clients, to let them know that the buckets have changed.
		push_hashmask_update(_buckets[i]); // all_hashmask(i, 0);
		
		_hashmasks[i] = calloc(1, sizeof(hashmask_t));
		assert(_hashmasks[i]);

		assert(_interface);
		_hashmasks[i]->primary = strdup(_interface);
		_hashmasks[i]->secondary = NULL;
	}

	// indicate that we have buckets that do not have backup copies on other nodes.
	_nobackup_buckets = _mask + 1;
	
	// we should have hashmasks setup as well by this point.
	assert(_hashmasks);
}



// we've been given a 'name' for a hash-key item, and so we lookup the bucket that is responsible 
// for that item.  the 'data' module will then find the data store within that handles that item.
int buckets_store_name_str(hash_t key_hash, char *name)
{
	hash_t bucket_index;
	bucket_t *bucket;

	assert(name);

	// calculate the bucket that this item belongs in.
	bucket_index = _mask & key_hash;
	assert(bucket_index <= _mask);
	bucket = _buckets[bucket_index];

	// if we have a record for this bucket, then we are either a primary or a backup for it.
	if (bucket) {
		assert(bucket->hash == bucket_index);
		
		// make sure that this server is 'primary' or 'secondary' for this bucket.
		assert(bucket->data);
		data_set_name_str(key_hash, bucket->data, name);
		return(0);
	}
	else {
		// we dont have the bucket, we need to let the other node know that something has gone wrong.
		return(-1);
	}
}


// we've been given a 'name' for a hash-key item, and so we lookup the bucket that is responsible 
// for that item.  the 'data' module will then find the data store within that handles that item.
int buckets_store_name_int(hash_t key_hash, long long int_key)
{
	hash_t bucket_index;
	bucket_t *bucket;

	// calculate the bucket that this item belongs in.
	bucket_index = _mask & key_hash;
	assert(bucket_index <= _mask);
	bucket = _buckets[bucket_index];

	// if we have a record for this bucket, then we are either a primary or a backup for it.
	if (bucket) {
		assert(bucket->hash == bucket_index);
		
		// make sure that this server is 'primary' or 'secondary' for this bucket.
		assert(bucket->data);
		data_set_name_int(key_hash, bucket->data, int_key);
		return(0);
	}
	else {
		// we dont have the bucket, we need to let the other node know that something has gone wrong.
		return(-1);
	}
}





static void bucket_dump(bucket_t *bucket)
{
	node_t *node;
	char *mode = NULL;
	char *altmode = NULL;
	char *altnode = NULL;
	
	assert(bucket);
	
	if (bucket->level == 0) {
		mode = "Primary";
		altmode = "Backup";
		assert(bucket->target_node == NULL);
		if (bucket->backup_node) {
			assert(bucket->backup_node->name);
			altnode = bucket->backup_node->name;
		}
		else {
			altnode = "";
		}
	}
	else if (bucket->level == 1) {
		mode = "Secondary";
		assert(bucket->backup_node == NULL);
		assert(bucket->target_node);
		assert(bucket->target_node->name);
		altmode = "Source";
		altnode = bucket->target_node->name;
	}
	else {
		mode = "Unknown";
		altmode = "Unknown";
		altnode = "";
	}

	assert(mode);
	assert(altmode);
	assert(altnode);
	stat_dumpstr("    Bucket:%#llx, Mode:%s, %s Node:%s", bucket->hash, mode, altmode, altnode);
	
	assert(bucket->data);
//	data_dump(bucket->data);

	if (bucket->transfer_client) {
		node = bucket->transfer_client->node;
		assert(node);
		assert(node->name);
		stat_dumpstr("      Currently transferring to: %s", node->name);
		stat_dumpstr("      Transfer Mode: %d", bucket->transfer_mode_special);
	}
}


void buckets_dump(void)
{
	int i;
	
	stat_dumpstr("BUCKETS");
	stat_dumpstr("  Mask: %#llx", _mask);
	stat_dumpstr("  Buckets without backups: %d", _nobackup_buckets);
	stat_dumpstr("  Primary Buckets: %d", _primary_buckets);
	stat_dumpstr("  Secondary Buckets: %d", _secondary_buckets);
	stat_dumpstr("  Bucket currently transferring: %s", _bucket_transfer == 0 ? "no" : "yes");
	stat_dumpstr("  Migration Sync Counter: %d", _migrate_sync);

	stat_dumpstr("  List of Buckets:");
	
	for (i=0; i<=_mask; i++) {
		if (_buckets[i]) {
			bucket_dump(_buckets[i]);
		}
	}
	stat_dumpstr(NULL);
}


void hashmasks_dump(void)
{
	hash_t i;

// the list of hashmasks is to know which servers are responsible for which buckets.
// this list of hashmasks will also replace the need to 'settle'.  Instead, a timed event will 
// occur periodically that checks that the hashmask coverage is complete.
// hashmask_t ** _hashmasks = NULL;

	assert(_hashmasks);
	
	stat_dumpstr("HASHMASKS");
	for (i=0; i<=_mask; i++) {
		assert(_hashmasks[i]);

		stat_dumpstr("  Hashmask:%#llx, Primary:'%s', Secondary:'%s'", i, 
					 _hashmasks[i]->primary ? _hashmasks[i]->primary : "",
					 _hashmasks[i]->secondary ? _hashmasks[i]->secondary : "");
	}
	
	stat_dumpstr(NULL);
}


// if the buckets are moving from primary to secondary, or the other way round, then the hashmasks 
// need to be switched to match it.
void hashmask_switch(hash_t hash) 
{
	char *tmp;
	
	assert(_hashmasks);
	assert(hash >= 0 && hash <= _mask);
	assert(_hashmasks[hash]);
	assert(_hashmasks[hash]->primary);
	assert(_hashmasks[hash]->secondary);
	
	tmp = _hashmasks[hash]->primary;
	_hashmasks[hash]->primary = _hashmasks[hash]->secondary;
	_hashmasks[hash]->secondary = tmp;
}





// Get the primary node for an external bucket.  If the bucket is being handled by this instance, then this 
// function will return NULL.  If it is being handled by another node, then it will return a string.
char * buckets_get_primary(hash_t key_hash) 
{
	int bucket_index;
	bucket_t *bucket;

	// calculate the bucket that this item belongs in.
	bucket_index = _mask & key_hash;
	assert(bucket_index >= 0);
	assert(bucket_index <= _mask);
	bucket = _buckets[bucket_index];
	if (bucket->target_node) {
		// that bucket is being handled 
		return(NULL);
	}
	else {
		assert(_hashmasks[bucket_index]);
		assert(_hashmasks[bucket_index]->primary);
		return(_hashmasks[bucket_index]->primary);
	}
}


