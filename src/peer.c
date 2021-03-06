/* Copyright (C) 2015-2017 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved. */

#include "peer.h"
#include "device.h"
#include "queueing.h"
#include "timers.h"
#include "hashtables.h"
#include "noise.h"

#include <linux/kref.h>
#include <linux/lockdep.h>
#include <linux/rcupdate.h>
#include <linux/list.h>

static atomic64_t peer_counter = ATOMIC64_INIT(0);

struct wireguard_peer *peer_create(struct wireguard_device *wg, const u8 public_key[NOISE_PUBLIC_KEY_LEN], const u8 preshared_key[NOISE_SYMMETRIC_KEY_LEN])
{
	struct wireguard_peer *peer;
	lockdep_assert_held(&wg->device_update_lock);

	if (peer_total_count(wg) >= MAX_PEERS_PER_DEVICE)
		return NULL;

	peer = kzalloc(sizeof(struct wireguard_peer), GFP_KERNEL);
	if (!peer)
		return NULL;
	peer->device = wg;

	if (dst_cache_init(&peer->endpoint_cache, GFP_KERNEL)) {
		kfree(peer);
		return NULL;
	}

	peer->internal_id = atomic64_inc_return(&peer_counter);
	peer->serial_work_cpu = nr_cpumask_bits;
	cookie_init(&peer->latest_cookie);
	if (!noise_handshake_init(&peer->handshake, &wg->static_identity, public_key, preshared_key, peer)) {
		kfree(peer);
		return NULL;
	}
	cookie_checker_precompute_peer_keys(peer);
	mutex_init(&peer->keypairs.keypair_update_lock);
	INIT_WORK(&peer->transmit_handshake_work, packet_handshake_send_worker);
	rwlock_init(&peer->endpoint_lock);
	kref_init(&peer->refcount);
	pubkey_hashtable_add(&wg->peer_hashtable, peer);
	list_add_tail(&peer->peer_list, &wg->peer_list);
	packet_queue_init(&peer->tx_queue, packet_tx_worker, false);
	packet_queue_init(&peer->rx_queue, packet_rx_worker, false);
	skb_queue_head_init(&peer->staged_packet_queue);
	timers_init(peer);
	pr_debug("%s: Peer %Lu created\n", wg->dev->name, peer->internal_id);
	return peer;
}

struct wireguard_peer *peer_get(struct wireguard_peer *peer)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_bh_held(), "Calling peer_get without holding the RCU read lock");
	if (unlikely(!peer || !kref_get_unless_zero(&peer->refcount)))
		return NULL;
	return peer;
}

struct wireguard_peer *peer_rcu_get(struct wireguard_peer *peer)
{
	rcu_read_lock_bh();
	peer = peer_get(peer);
	rcu_read_unlock_bh();
	return peer;
}

/* We have a separate "remove" function to get rid of the final reference because
 * peer_list, clearing handshakes, and flushing all require mutexes which requires
 * sleeping, which must only be done from certain contexts. */
void peer_remove(struct wireguard_peer *peer)
{
	if (unlikely(!peer))
		return;
	lockdep_assert_held(&peer->device->device_update_lock);
	noise_handshake_clear(&peer->handshake);
	noise_keypairs_clear(&peer->keypairs);
	list_del(&peer->peer_list);
	timers_stop(peer);
	routing_table_remove_by_peer(&peer->device->peer_routing_table, peer);
	pubkey_hashtable_remove(&peer->device->peer_hashtable, peer);
	skb_queue_purge(&peer->staged_packet_queue);
	flush_workqueue(peer->device->packet_crypt_wq); /* The first flush is for encrypt/decrypt step. */
	flush_workqueue(peer->device->packet_crypt_wq); /* The second flush is for send/receive step. */
	flush_workqueue(peer->device->handshake_send_wq);
	peer_put(peer);
}

static void rcu_release(struct rcu_head *rcu)
{
	struct wireguard_peer *peer = container_of(rcu, struct wireguard_peer, rcu);
	pr_debug("%s: Peer %Lu (%pISpfsc) destroyed\n", peer->device->dev->name, peer->internal_id, &peer->endpoint.addr);
	skb_queue_purge(&peer->staged_packet_queue);
	dst_cache_destroy(&peer->endpoint_cache);
	kzfree(peer);
}

static void kref_release(struct kref *refcount)
{
	struct wireguard_peer *peer = container_of(refcount, struct wireguard_peer, refcount);
	call_rcu_bh(&peer->rcu, rcu_release);
}

void peer_put(struct wireguard_peer *peer)
{
	if (unlikely(!peer))
		return;
	kref_put(&peer->refcount, kref_release);
}

void peer_remove_all(struct wireguard_device *wg)
{
	struct wireguard_peer *peer, *temp;
	lockdep_assert_held(&wg->device_update_lock);
	list_for_each_entry_safe (peer, temp, &wg->peer_list, peer_list)
		peer_remove(peer);
}

unsigned int peer_total_count(struct wireguard_device *wg)
{
	unsigned int i = 0;
	struct wireguard_peer *peer;
	lockdep_assert_held(&wg->device_update_lock);
	list_for_each_entry (peer, &wg->peer_list, peer_list)
		++i;
	return i;
}
