/* This file contains functions that handle incoming Homa messages, including
 * both receiving information for those messages and sending grants. */

#include "homa_impl.h"

/**
 * homa_message_in_init() - Constructor for homa_message_in.
 * @msgin:        Structure to initialize.
 * @length:       Total number of bytes in message.
 * @unscheduled:  Initial bytes of message that will be sent without grants.
 */
void homa_message_in_init(struct homa_message_in *msgin, int length,
		int unscheduled)
{
	msgin->total_length = length;
	__skb_queue_head_init(&msgin->packets);
	msgin->bytes_remaining = length;
	msgin->granted = unscheduled;
	if (msgin->granted > msgin->total_length)
		msgin->granted = msgin->total_length;
	msgin->priority = 0;
	msgin->scheduled = length > unscheduled;
	msgin->possibly_in_grant_queue = msgin->scheduled;
	if (length <= 4096) {
		INC_METRIC(small_msg_bytes[(length-1) >> 6], length);
	} else if (length <= 0x10000) {
		INC_METRIC(medium_msg_bytes[(length-1) >> 10], length);
	} else {
		INC_METRIC(large_msg_bytes, length);
	}
}

/**
 * homa_message_in_destroy() - Destructor for homa_message_in.
 * @msgin:       Structure to clean up.
 */
void homa_message_in_destroy(struct homa_message_in *msgin)
{
	struct sk_buff *skb, *next;
	if (msgin->total_length < 0)
		return;
	skb_queue_walk_safe(&msgin->packets, skb, next) {
		kfree_skb(skb);
	}
	__skb_queue_head_init(&msgin->packets);
	msgin->total_length = -1;
}

/**
 * homa_add_packet() - Add an incoming packet to the contents of a
 * partially received message.
 * @msgin: Overall information about the incoming message.
 * @skb:   The new packet. This function takes ownership of the packet
 *         and will free it, if it doesn't get added to msgin (because
 *         it provides no new data).
 */
void homa_add_packet(struct homa_message_in *msgin, struct sk_buff *skb)
{
	struct data_header *h = (struct data_header *) skb->data;
	int offset = ntohl(h->offset);
	struct sk_buff *skb2;
	
	/* Any data from the packet with offset less than this is
	 * of no value.*/
	int floor = 0;
	
	/* Any data with offset >= this is useless. */
	int ceiling = msgin->total_length;
	
	/* Figure out where in the list of existing packets to insert the
	 * new one. It doesn't necessarily go at the end, but it almost
	 * always will in practice, so work backwards from the end of the
	 * list.
	 */
	skb_queue_reverse_walk(&msgin->packets, skb2) {
		struct data_header *h2 = (struct data_header *) skb2->data;
		int offset2 = ntohl(h2->offset);
		if (offset2 < offset) {
			floor = offset2 + HOMA_MAX_DATA_PER_PACKET;
			break;
		}
		ceiling = offset2;
	} 
		
	/* New packet goes right after skb2 (which may refer to the header).
	 * Packets shouldn't overlap in byte ranges, but the code below
	 * assumes they might, so it computes how many non-overlapping bytes
	 * are contributed by the new packet.
	 */
	if (unlikely(floor < offset)) {
		floor = offset;
	}
	if (ceiling > offset + HOMA_MAX_DATA_PER_PACKET) {
		ceiling = offset + HOMA_MAX_DATA_PER_PACKET;
	}
	if (floor >= ceiling) {
		/* This packet is redundant. */
		char buffer[100];
		printk(KERN_NOTICE "redundant Homa packet: %s\n",
			homa_print_packet(skb, buffer, sizeof(buffer)));
		kfree_skb(skb);
		return;
	}
	__skb_insert(skb, skb2, skb2->next, &msgin->packets);
	msgin->bytes_remaining -= (ceiling - floor);
}

/**
 * homa_message_in_copy_data() - Extract the data from an incoming message
 * and copy it to buffer(s) in user space.
 * @msgin:      The message whose data should be extracted.
 * @iter:       Describes the available buffer space at user-level; message
 *              data gets copied here.
 * @max_bytes:  Total amount of space available via iter.
 * 
 * Return:      The number of bytes copied, or a negative errno.
 */
int homa_message_in_copy_data(struct homa_message_in *msgin,
		struct iov_iter *iter, int max_bytes)
{
	struct sk_buff *skb;
	int offset;
	int err;
	int remaining = max_bytes;
	
	/* Do the right thing even if packets have overlapping ranges.
	 * Honestly, though, this shouldn't happen.
	 */
	offset = 0;
	skb_queue_walk(&msgin->packets, skb) {
		struct data_header *h = (struct data_header *) skb->data;
		int this_offset = ntohl(h->offset);
		int this_size = msgin->total_length - offset;
		if (this_size > HOMA_MAX_DATA_PER_PACKET) {
			this_size = HOMA_MAX_DATA_PER_PACKET;
		}
		if (offset > this_offset) {
			this_size -= (offset - this_offset);
		}
		if (this_size > remaining) {
			this_size =  remaining;
		}
		err = skb_copy_datagram_iter(skb,
				sizeof(*h) + (offset - this_offset),
				iter, this_size);
		if (err) {
			return err;
		}
		remaining -= this_size;
		offset += this_size;
		if (remaining == 0) {
			break;
		}
	}
	return max_bytes - remaining;
}

/**
 * homa_get_resend_range() - Given a message for which some input data
 * is missing, find the first range of missing data.
 * @msgin:     Message for which not all granted data has been received.
 * @resend:    The @offset and @length fields of this structure will be
 *             filled in with information about the first missing range
 *             in @msgin.
 */
void homa_get_resend_range(struct homa_message_in *msgin,
		struct resend_header *resend)
{
	struct sk_buff *skb;
	int missing_bytes;
	/* This will eventually be the top of the first missing range. */
	int end_offset;
	
	if (msgin->total_length < 0) {
		/* Haven't received any data for this message; request
		 * retransmission of just the first packet.
		 */
		resend->offset = 0;
		resend->length = htonl(HOMA_MAX_DATA_PER_PACKET);
		return;
	}
	
	missing_bytes = msgin->bytes_remaining
			- (msgin->total_length - msgin->granted);
	end_offset = msgin->granted;
	
	/* Basic idea: walk backwards through the message's packets until
	 * we have accounted for all missing bytes; this will identify
	 * the first missing range.
	 */
	skb_queue_reverse_walk(&msgin->packets, skb) {
		int offset = ntohl(((struct data_header *) skb->data)->offset);
		int pkt_length, gap;
		
		pkt_length = msgin->total_length - offset;
		if (pkt_length > HOMA_MAX_DATA_PER_PACKET) {
			pkt_length = HOMA_MAX_DATA_PER_PACKET;
		}
		gap = end_offset - (offset + pkt_length);
		missing_bytes -= gap;
		if (missing_bytes == 0) {
			resend->offset = htonl(offset + pkt_length);
			resend->length = htonl(gap);
			return;
		}
		end_offset = offset;
	}
	
	/* The first packet(s) are missing. */
	resend->offset = 0;
	resend->length = htonl(missing_bytes);
}

/**
 * homa_pkt_dispatch() - Top-level function for handling an incoming packet,
 * once its socket has been found and locked.
 * @sk:     Homa socket that owns the packet's destination port. Caller must
 *          own the spin lock for this and socket must not have been deleted.
 * @skb:    The incoming packet. This function takes ownership of the packet
 *          (we'll ensure that it is eventually freed).
 *
 * Return:  Always returns 0.
 */
int homa_pkt_dispatch(struct sock *sk, struct sk_buff *skb)
{
	struct homa_sock *hsk = homa_sk(sk);
	struct common_header *h = (struct common_header *) skb->data;
	struct homa_rpc *rpc;
	
	if (ntohs(h->dport) < HOMA_MIN_CLIENT_PORT) {
		/* We are the server for this RPC. */
		rpc = homa_find_server_rpc(hsk, ip_hdr(skb)->saddr,
				ntohs(h->sport), h->id);
		if ((rpc == NULL) && (h->type == DATA)) {
			/* New incoming RPC. */
			rpc = homa_rpc_new_server(hsk, ip_hdr(skb)->saddr,
					(struct data_header *) h);
			if (IS_ERR(rpc)) {
				printk(KERN_WARNING "homa_pkt_dispatch "
						"couldn't create server rpc: "
						"error %lu",
						-PTR_ERR(rpc));
				INC_METRIC(server_cant_create_rpcs, 1);
				goto discard;
			}
			INC_METRIC(requests_received, 1);
		}
	} else {
		rpc = homa_find_client_rpc(hsk, ntohs(h->sport), h->id);
	}
	if (unlikely(rpc == NULL)) {
		if ((h->type != CUTOFFS) && (h->type != RESEND)) {
			INC_METRIC(unknown_rpcs, 1);
			goto discard;
		}
	} else {
		rpc->silent_ticks = 0;
	}
	
	switch (h->type) {
	case DATA:
		INC_METRIC(packets_received[DATA - DATA], 1);
		homa_data_pkt(skb, rpc);
		break;
	case GRANT:
		INC_METRIC(packets_received[GRANT - DATA], 1);
		homa_grant_pkt(skb, rpc);
		break;
	case RESEND:
		INC_METRIC(packets_received[RESEND - DATA], 1);
		homa_resend_pkt(skb, rpc, hsk);
		break;
	case RESTART:
		INC_METRIC(packets_received[RESTART - DATA], 1);
		homa_restart_pkt(skb, rpc);
		break;
	case BUSY:
		INC_METRIC(packets_received[BUSY - DATA], 1);
		/* Nothing to do for these packets except reset silent_ticks,
		 * which happened above.
		 */
		goto discard;
	case CUTOFFS:
		INC_METRIC(packets_received[CUTOFFS - DATA], 1);
		homa_cutoffs_pkt(skb, hsk);
		break;
	default:
		INC_METRIC(unknown_packet_types, 1);
		goto discard;
	}
	return 0;
	
    discard:
	kfree_skb(skb);
	return 0;
}

/**
 * homa_data_pkt() - Handler for incoming DATA packets
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 * 
 * This method may change the RPC's state to RPC_READY.
 */
void homa_data_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
{
	struct homa *homa = rpc->hsk->homa;
	struct data_header *h = (struct data_header *) skb->data;
	if (rpc->state != RPC_INCOMING) {
		if (unlikely(!rpc->is_client || (rpc->state == RPC_READY))) {
			kfree_skb(skb);
			return;			
		}
		homa_message_in_init(&rpc->msgin, ntohl(h->message_length),
				ntohl(h->unscheduled));
		INC_METRIC(responses_received, 1);
		rpc->state = RPC_INCOMING;
	}
	homa_add_packet(&rpc->msgin, skb);
	if (rpc->msgin.scheduled)
		homa_manage_grants(homa, rpc);
	if (rpc->msgin.bytes_remaining == 0) {
		struct sock *sk = (struct sock *) rpc->hsk;
		rpc->state = RPC_READY;
		list_add_tail(&rpc->ready_links, &rpc->hsk->ready_rpcs);
		sk->sk_data_ready(sk);
	}
	if (ntohs(h->cutoff_version) != homa->cutoff_version) {
		/* The sender has out-of-date cutoffs. Note: we may need
		 * to resend CUTOFFS packets if one gets lost, but we don't
		 * want to send multiple CUTOFFS packets when a stream of
		 * packets arrives with stale cutoff_versions. Thus, we
		 * don't send CUTOFFS unless there is a version mismatch
		 * *and* it is been a while since the previous CUTOFFS
		 * packet.
		 */
		if (jiffies != rpc->peer->last_update_jiffies) {
			struct cutoffs_header h2;
			int i;
			
			for (i = 0; i < HOMA_NUM_PRIORITIES; i++) {
				h2.unsched_cutoffs[i] =
						htonl(homa->unsched_cutoffs[i]);
			}
			h2.cutoff_version = htons(homa->cutoff_version);
			homa_xmit_control(CUTOFFS, &h2, sizeof(h2), rpc);
			rpc->peer->last_update_jiffies = jiffies;
		}
	}
}

/**
 * homa_grant_pkt() - Handler for incoming GRANT packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 */
void homa_grant_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
{
	struct grant_header *h = (struct grant_header *) skb->data;
	
	if (rpc->state == RPC_OUTGOING) {
		int new_offset = ntohl(h->offset);

		if (new_offset > rpc->msgout.granted) {
			rpc->msgout.granted = new_offset;
			if (new_offset > rpc->msgout.length)
				rpc->msgout.granted = rpc->msgout.length;
		}
		rpc->msgout.sched_priority = h->priority;
		homa_xmit_data(&rpc->msgout, (struct sock *) rpc->hsk,
				rpc->peer);
	}
	kfree_skb(skb);
}

/**
 * homa_resend_pkt() - Handler for incoming RESEND packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet; NULL
 *           if the packet doesn't belong to an existing RPC.
 * @hsk:     Socket on which the packet was received.
 */
void homa_resend_pkt(struct sk_buff *skb, struct homa_rpc *rpc,
		struct homa_sock *hsk)
{
	struct resend_header *h = (struct resend_header *) skb->data;
	struct busy_header busy;

	if (ntohs(h->common.dport) < HOMA_MIN_CLIENT_PORT) {
		/* We are the server for this RPC. */
		if (rpc == NULL) {
			/* Send RESTART. */
			struct restart_header restart;
			struct homa_peer *peer;
			restart.common.sport = h->common.dport;
			restart.common.dport = h->common.sport;
			restart.common.id = h->common.id;
			restart.common.type = RESTART;
			peer = homa_peer_find(&hsk->homa->peers,
					ip_hdr(skb)->saddr, &hsk->inet);
			if (IS_ERR(peer))
				goto done;
			__homa_xmit_control(&restart, sizeof(restart), peer,
					hsk);
			goto done;
		}
		if (rpc->state != RPC_OUTGOING) {
			homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
			goto done;
		}
	} else {
		/* We are the client for this RPC. */
		if ((rpc == NULL) || (rpc->state != RPC_OUTGOING))
			goto done;
	}
	if (rpc->msgout.next_offset < rpc->msgout.granted) {
		/* We have chosen not to transmit data from this message;
		 * send BUSY instead.
		 */
		homa_xmit_control(BUSY, &busy, sizeof(busy), rpc);
	} else {
		homa_resend_data(&rpc->msgout, ntohl(h->offset),
				ntohl(h->offset) + ntohl(h->length),
				(struct sock *) rpc->hsk, rpc->peer,
				h->priority);
	}
		
    done:
	kfree_skb(skb);
}

/**
 * homa_restart_pkt() - Handler for incoming RESTART packets.
 * @skb:     Incoming packet; size known to be large enough for the header.
 *           This function now owns the packet.
 * @rpc:     Information about the RPC corresponding to this packet.
 */
void homa_restart_pkt(struct sk_buff *skb, struct homa_rpc *rpc)
{
	homa_remove_from_grantable(rpc->hsk->homa, rpc);
	homa_message_in_destroy(&rpc->msgin);
	homa_message_out_reset(&rpc->msgout);
	rpc->state = RPC_OUTGOING;
	homa_xmit_data(&rpc->msgout, (struct sock *) rpc->hsk, rpc->peer);
	kfree_skb(skb);
}

/**
 * homa_cutoffs_pkt() - Handler for incoming CUTOFFS packets
 * @skb:     Incoming packet; size already verified large enough for header.
 *           This function now owns the packet.
 * @hsk:     Socket on which the packet was received.
 */
void homa_cutoffs_pkt(struct sk_buff *skb, struct homa_sock *hsk)
{
	int i;
	struct cutoffs_header *h = (struct cutoffs_header *) skb->data;
	struct homa_peer *peer = homa_peer_find(&hsk->homa->peers,
		ip_hdr(skb)->saddr, &hsk->inet);
	
	if (!IS_ERR(peer)) {
		peer->unsched_cutoffs[0] = INT_MAX;
		for (i = 1; i <HOMA_NUM_PRIORITIES; i++)
			peer->unsched_cutoffs[i] = ntohl(h->unsched_cutoffs[i]);
		peer->cutoff_version = h->cutoff_version;
	}
	kfree_skb(skb);
}

/**
 * homa_manage_grants() - This function is invoked to set priorities of
 * messages for grants, determine whether grants can be sent out and, if so,
 * send them.
 * @homa:    Overall data about the Homa protocol implementation.
 * @rpc:     If non-null, this is an RPC whose msgin just received a packet;
 *           rpc->msgin->scheduled should be true.  This RPC may need to
 *           be (re-)positioned in the grant queue. NULL typically means
 *           an RPC has just been removed from the queue, which may allow
 *           grants to be sent for other RPCs.
 */
void homa_manage_grants(struct homa *homa, struct homa_rpc *rpc)
{
	/* The overall goal is to grant simultaneously to up to
	 * homa->max_overcommit messages. Ideally, each message should use
	 * a different priority level, determined by bytes_remaining (fewest
	 * bytes_remaining gets the highest priority). If there aren't enough
	 * scheduled priority levels for all of the messages, then the lowest
	 * level gets shared by multiple messages. If there are fewer messages
	 * than priority levels, then we use the lowest available levels
	 * (new higher-priority messages can use the higher levels to achieve
	 * instantaneous preemption).
	 */
	struct list_head *pos;
	struct homa_rpc *candidate;
	int rank;
	struct homa_message_in *msgin = &rpc->msgin;
	static int invocation = 0;
	
	spin_lock_bh(&homa->grantable_lock);
	invocation++;
	
	if (!rpc)
		goto check_grant;
	
	/* First, make sure this message is in the right place in (or not in)
	 * homa->grantable_msgs.
	 */
	if (msgin->granted >= msgin->total_length) {
		/* Message fully granted; no need to track it anymore. */
		if (!list_empty(&rpc->grantable_links)) {
			homa->num_grantable--;
			list_del_init(&rpc->grantable_links);
		}
		msgin->possibly_in_grant_queue = 0;
	} else if (list_empty(&rpc->grantable_links)) {
		/* Message not yet tracked; add it in priority order. */
		homa->num_grantable++;
		list_for_each(pos, &homa->grantable_rpcs) {
			candidate = list_entry(pos, struct homa_rpc,
					grantable_links);
			if (candidate->msgin.bytes_remaining
					> msgin->bytes_remaining) {
				list_add_tail(&rpc->grantable_links, pos);
				goto check_grant;
			}
		}
		list_add_tail(&rpc->grantable_links, &homa->grantable_rpcs);
	} else while (homa->grantable_rpcs.next != &rpc->grantable_links) {
		/* Message is on the list, but its priority may have
		 * increased because of the recent packet arrival. If so,
		 * adjust its position in the list.
		 */
		candidate = list_prev_entry(rpc, grantable_links);
		if (candidate->msgin.bytes_remaining <= msgin->bytes_remaining)
			goto check_grant;
		__list_del_entry(&candidate->grantable_links);
		list_add(&candidate->grantable_links, &rpc->grantable_links);
	}
	
    check_grant:
	/* Next, see if there are any messages that deserve a grant (they have
	 * fewer than homa->rtt_bytes of data that have been granted but
	 * not yet received). */
	rank = 0;
	list_for_each(pos, &homa->grantable_rpcs) {
		int extra_levels, priority;
		int desired_grant;
		struct grant_header h;
		
		rank++;
		if (rank > homa->max_overcommit) {
			break;
		}
		candidate = list_entry(pos, struct homa_rpc, grantable_links);
		desired_grant = homa->rtt_bytes + candidate->msgin.total_length
				- candidate->msgin.bytes_remaining;
		if (desired_grant > candidate->msgin.total_length)
			desired_grant = candidate->msgin.total_length;
		if (candidate->msgin.granted >= desired_grant)
			continue;
		
		/* Send a grant for this message. */
		candidate->msgin.granted = desired_grant;
		h.offset = htonl(candidate->msgin.granted);
		priority = homa->max_sched_prio - (rank - 1);
		extra_levels = (homa->max_sched_prio - homa->min_prio + 1)
				- homa->num_grantable;
		if (extra_levels >= 0)
			priority -= extra_levels;
		else if (priority < homa->min_prio)
			priority = homa->min_prio;
		h.priority = priority;
		if (!homa_xmit_control(GRANT, &h, sizeof(h), candidate)) {
			/* Don't do anything if the grant couldn't be sent; let
			 * other retry mechanisms handle this. */
		}
	}
	spin_unlock_bh(&homa->grantable_lock);
}

/**
 * homa_remove_from_grantable() - This method ensures that an RPC
 * is no longer linked into homa->grantable_rpcs (i.e. it won't be
 * visible to homa_manage_grants).
 * @homa:    Overall data about the Homa protocol implementation.
 * @rpc:     RPC that is being destroyed.
 */
void homa_remove_from_grantable(struct homa *homa, struct homa_rpc *rpc)
{
	UNIT_LOG("; ", "homa_remove_from_grantable invoked");
	if (rpc->msgin.possibly_in_grant_queue
			&& (rpc->msgin.total_length >= 0)) {
		spin_lock_bh(&homa->grantable_lock);
		if (!list_empty(&rpc->grantable_links)) {
			homa->num_grantable--;
			list_del_init(&rpc->grantable_links);
			spin_unlock_bh(&homa->grantable_lock);
			homa_manage_grants(homa, NULL);
			return;
		}
		spin_unlock_bh(&homa->grantable_lock);
	}
}

/**
 * homa_rpc_abort() - Terminate an RPC and arrange for an error to be returned
 * to the application.
 * @crpc:    RPC to be terminated. Must be a client RPC (@is_client != 0).
 * @error:   A negative errno value indicating the error that caused the abort.
 */
void homa_rpc_abort(struct homa_rpc *crpc, int error)
{
	struct sock *sk = (struct sock *) crpc->hsk;
	homa_remove_from_grantable(crpc->hsk->homa, crpc);
	crpc->error = error;
	crpc->state = RPC_READY;
	list_add_tail(&crpc->ready_links, &crpc->hsk->ready_rpcs);
	sk->sk_data_ready(sk);
}

/**
 * homa_validate_grantable_list() - Scan the grantable_rpcs list to
 * see if it has somehow gotten looped back on itself. This function
 * is intended for debugging.
 * @homa:    Overall data about the Homa protocol implementation.
 * @message: Text to include in message printed if a circularity is
 *           found. Typically identifies the caller of this function.
 */
void homa_validate_grantable_list(struct homa *homa, char *message) {
	struct list_head *pos;
	struct homa_rpc *rpc, *first;
	int count = 0;
	first = 0;
	list_for_each(pos, &homa->grantable_rpcs) {
		count++;
		if (count == 1000000) {
			printk(KERN_NOTICE "Grantable list has %d entries!\n",
				count);
			return;
		}
		rpc = list_entry(pos, struct homa_rpc,
				grantable_links);
		if (first == NULL) {
			first = rpc;
		} else if (rpc == first) {
			printk(KERN_NOTICE "Circular grant list in %s\n",
				message);
			BUG();
		}
	}
}