/* Copyright (c) 2017 - 2019 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_send_ctl.c -- Logic for sending and sent packets
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "lsquic_types.h"
#include "lsquic_int_types.h"
#include "lsquic.h"
#include "lsquic_mm.h"
#include "lsquic_engine_public.h"
#include "lsquic_packet_common.h"
#include "lsquic_alarmset.h"
#include "lsquic_parse.h"
#include "lsquic_packet_out.h"
#include "lsquic_senhist.h"
#include "lsquic_rtt.h"
#include "lsquic_cubic.h"
#include "lsquic_pacer.h"
#include "lsquic_bw_sampler.h"
#include "lsquic_minmax.h"
#include "lsquic_bbr.h"
#include "lsquic_send_ctl.h"
#include "lsquic_util.h"
#include "lsquic_sfcw.h"
#include "lsquic_varint.h"
#include "lsquic_hq.h"
#include "lsquic_hash.h"
#include "lsquic_stream.h"
#include "lsquic_ver_neg.h"
#include "lsquic_ev_log.h"
#include "lsquic_conn.h"
#include "lsquic_conn_flow.h"
#include "lsquic_conn_public.h"
#include "lsquic_cong_ctl.h"
#include "lsquic_enc_sess.h"
#include "lsquic_hash.h"
#include "lsquic_malo.h"
#include "lsquic_attq.h"

#define LSQUIC_LOGGER_MODULE LSQLM_SENDCTL
#define LSQUIC_LOG_CONN_ID lsquic_conn_log_cid(ctl->sc_conn_pub->lconn)
#include "lsquic_logger.h"

#define MAX_RESUBMITTED_ON_RTO  2
#define MAX_RTO_BACKOFFS        10
#define DEFAULT_RETX_DELAY      500000      /* Microseconds */
#define MAX_RTO_DELAY           60000000    /* Microseconds */
#define MIN_RTO_DELAY           1000000      /* Microseconds */
#define N_NACKS_BEFORE_RETX     3

#define CGP(ctl) ((struct cong_ctl *) &(ctl)->sc_cong_u)

#define packet_out_total_sz(p) \
                lsquic_packet_out_total_sz(ctl->sc_conn_pub->lconn, p)
#define packet_out_sent_sz(p) \
                lsquic_packet_out_sent_sz(ctl->sc_conn_pub->lconn, p)

enum retx_mode {
    RETX_MODE_HANDSHAKE,
    RETX_MODE_LOSS,
    RETX_MODE_TLP,
    RETX_MODE_RTO,
};


static const char *const retx2str[] = {
    [RETX_MODE_HANDSHAKE] = "RETX_MODE_HANDSHAKE",
    [RETX_MODE_LOSS]      = "RETX_MODE_LOSS",
    [RETX_MODE_TLP]       = "RETX_MODE_TLP",
    [RETX_MODE_RTO]       = "RETX_MODE_RTO",
};

#ifdef NDEBUG
#define MAX_BPQ_COUNT 10
#else
static unsigned MAX_BPQ_COUNT = 10;
void
lsquic_send_ctl_set_max_bpq_count (unsigned count) { MAX_BPQ_COUNT = count; }
#endif


static void
update_for_resending (lsquic_send_ctl_t *ctl, lsquic_packet_out_t *packet_out);


enum expire_filter { EXFI_ALL, EXFI_HSK, EXFI_LAST, };


static void
send_ctl_expire (struct lsquic_send_ctl *, enum packnum_space,
                                                        enum expire_filter);

static void
set_retx_alarm (struct lsquic_send_ctl *, enum packnum_space, lsquic_time_t);

static void
send_ctl_detect_losses (struct lsquic_send_ctl *, enum packnum_space,
                                                        lsquic_time_t time);

static unsigned
send_ctl_retx_bytes_out (const struct lsquic_send_ctl *ctl);

static unsigned
send_ctl_all_bytes_out (const struct lsquic_send_ctl *ctl);


#ifdef NDEBUG
static
#elif __GNUC__
__attribute__((weak))
#endif
int
lsquic_send_ctl_schedule_stream_packets_immediately (lsquic_send_ctl_t *ctl)
{
    return !(ctl->sc_flags & SC_BUFFER_STREAM);
}


#ifdef NDEBUG
static
#elif __GNUC__
__attribute__((weak))
#endif
enum packno_bits
lsquic_send_ctl_guess_packno_bits (lsquic_send_ctl_t *ctl)
{
    return PACKNO_BITS_1;   /* This is 2 bytes in both GQUIC and IQUIC */
}


int
lsquic_send_ctl_have_unacked_stream_frames (const lsquic_send_ctl_t *ctl)
{
    const lsquic_packet_out_t *packet_out;

    TAILQ_FOREACH(packet_out, &ctl->sc_unacked_packets[PNS_APP], po_next)
        if (0 == (packet_out->po_flags & PO_LOSS_REC)
                && (packet_out->po_frame_types &
                    ((1 << QUIC_FRAME_STREAM) | (1 << QUIC_FRAME_RST_STREAM))))
            return 1;

    return 0;
}


static lsquic_packet_out_t *
send_ctl_first_unacked_retx_packet (const struct lsquic_send_ctl *ctl,
                                                        enum packnum_space pns)
{
    lsquic_packet_out_t *packet_out;

    TAILQ_FOREACH(packet_out, &ctl->sc_unacked_packets[pns], po_next)
        if (0 == (packet_out->po_flags & PO_LOSS_REC)
                && (packet_out->po_frame_types & ctl->sc_retx_frames))
            return packet_out;

    return NULL;
}


static lsquic_packet_out_t *
send_ctl_last_unacked_retx_packet (const struct lsquic_send_ctl *ctl,
                                                    enum packnum_space pns)
{
    lsquic_packet_out_t *packet_out;
    TAILQ_FOREACH_REVERSE(packet_out, &ctl->sc_unacked_packets[pns],
                                            lsquic_packets_tailq, po_next)
        if (0 == (packet_out->po_flags & PO_LOSS_REC)
                && (packet_out->po_frame_types & ctl->sc_retx_frames))
            return packet_out;
    return NULL;
}


static int
have_unacked_handshake_packets (const lsquic_send_ctl_t *ctl)
{
    const lsquic_packet_out_t *packet_out;
    enum packnum_space pns;

    for (pns = ctl->sc_flags & SC_IETF ? PNS_INIT : PNS_APP; pns < N_PNS; ++pns)
        TAILQ_FOREACH(packet_out, &ctl->sc_unacked_packets[pns], po_next)
            if (packet_out->po_flags & PO_HELLO)
                return 1;
    return 0;
}


static enum retx_mode
get_retx_mode (const lsquic_send_ctl_t *ctl)
{
    if (!(ctl->sc_conn_pub->lconn->cn_flags & LSCONN_HANDSHAKE_DONE)
                                    && have_unacked_handshake_packets(ctl))
        return RETX_MODE_HANDSHAKE;
    if (ctl->sc_loss_to)
        return RETX_MODE_LOSS;
    if (ctl->sc_n_tlp < 2)
        return RETX_MODE_TLP;
    return RETX_MODE_RTO;
}


static lsquic_time_t
get_retx_delay (const struct lsquic_rtt_stats *rtt_stats)
{
    lsquic_time_t srtt, delay;

    srtt = lsquic_rtt_stats_get_srtt(rtt_stats);
    if (srtt)
    {
        delay = srtt + 4 * lsquic_rtt_stats_get_rttvar(rtt_stats);
        if (delay < MIN_RTO_DELAY)
            delay = MIN_RTO_DELAY;
    }
    else
        delay = DEFAULT_RETX_DELAY;

    return delay;
}


static void
retx_alarm_rings (enum alarm_id al_id, void *ctx, lsquic_time_t expiry, lsquic_time_t now)
{
    lsquic_send_ctl_t *ctl = ctx;
    lsquic_packet_out_t *packet_out;
    enum packnum_space pns;
    enum retx_mode rm;

    pns = al_id - AL_RETX_INIT;

    /* This is a callback -- before it is called, the alarm is unset */
    assert(!lsquic_alarmset_is_set(ctl->sc_alset, AL_RETX_INIT + pns));

    rm = get_retx_mode(ctl);
    LSQ_INFO("retx timeout, mode %s", retx2str[rm]);

    switch (rm)
    {
    case RETX_MODE_HANDSHAKE:
        send_ctl_expire(ctl, pns, EXFI_HSK);
        /* Do not register cubic loss during handshake */
        break;
    case RETX_MODE_LOSS:
        send_ctl_detect_losses(ctl, pns, now);
        break;
    case RETX_MODE_TLP:
        ++ctl->sc_n_tlp;
        send_ctl_expire(ctl, pns, EXFI_LAST);
        break;
    case RETX_MODE_RTO:
        ctl->sc_last_rto_time = now;
        ++ctl->sc_n_consec_rtos;
        ctl->sc_next_limit = 2;
        LSQ_DEBUG("packet RTO is %"PRIu64" usec", expiry);
        send_ctl_expire(ctl, pns, EXFI_ALL);
        ctl->sc_ci->cci_timeout(CGP(ctl));
        break;
    }

    packet_out = send_ctl_first_unacked_retx_packet(ctl, pns);
    if (packet_out)
        set_retx_alarm(ctl, pns, now);
    lsquic_send_ctl_sanity_check(ctl);
}


static lsquic_packno_t
first_packno (const struct lsquic_send_ctl *ctl)
{
    if (ctl->sc_flags & SC_IETF)
        return 0;
    else
        return 1;
}


/*
 * [draft-ietf-quic-transport-12], Section 4.4.1:
 *
 * "   The first Initial packet that is sent by a client contains a packet
 * "   number of 0.  All subsequent packets contain a packet number that is
 * "   incremented by at least one, see (Section 4.8).
 */
static void
send_ctl_pick_initial_packno (struct lsquic_send_ctl *ctl)
{
    ctl->sc_cur_packno = first_packno(ctl) - 1;
}


void
lsquic_send_ctl_init (lsquic_send_ctl_t *ctl, struct lsquic_alarmset *alset,
          struct lsquic_engine_public *enpub, const struct ver_neg *ver_neg,
          struct lsquic_conn_public *conn_pub, enum send_ctl_flags flags)
{
    unsigned i, algo;
    memset(ctl, 0, sizeof(*ctl));
    TAILQ_INIT(&ctl->sc_scheduled_packets);
    TAILQ_INIT(&ctl->sc_unacked_packets[PNS_INIT]);
    TAILQ_INIT(&ctl->sc_unacked_packets[PNS_HSK]);
    TAILQ_INIT(&ctl->sc_unacked_packets[PNS_APP]);
    TAILQ_INIT(&ctl->sc_lost_packets);
    ctl->sc_enpub = enpub;
    ctl->sc_alset = alset;
    ctl->sc_ver_neg = ver_neg;
    ctl->sc_conn_pub = conn_pub;
    assert(!(flags & ~(SC_IETF|SC_NSTP|SC_ECN)));
    ctl->sc_flags = flags;
    send_ctl_pick_initial_packno(ctl);
    if (enpub->enp_settings.es_pace_packets)
        ctl->sc_flags |= SC_PACE;
    if (flags & SC_ECN)
        ctl->sc_ecn = ECN_ECT0;
    else
        ctl->sc_ecn = ECN_NOT_ECT;
    if (flags & SC_IETF)
        ctl->sc_retx_frames = IQUIC_FRAME_RETX_MASK;
    else
        ctl->sc_retx_frames = GQUIC_FRAME_RETRANSMITTABLE_MASK;
    lsquic_alarmset_init_alarm(alset, AL_RETX_INIT, retx_alarm_rings, ctl);
    lsquic_alarmset_init_alarm(alset, AL_RETX_HSK, retx_alarm_rings, ctl);
    lsquic_alarmset_init_alarm(alset, AL_RETX_APP, retx_alarm_rings, ctl);
    lsquic_senhist_init(&ctl->sc_senhist, ctl->sc_flags & SC_IETF);
    if (0 == enpub->enp_settings.es_cc_algo)
        algo = LSQUIC_DF_CC_ALGO;
    else
        algo = enpub->enp_settings.es_cc_algo;
    if (algo == 2)
        ctl->sc_ci = &lsquic_cong_bbr_if;
    else
        ctl->sc_ci = &lsquic_cong_cubic_if;
    ctl->sc_ci->cci_init(CGP(ctl), conn_pub, ctl->sc_retx_frames);
    if (ctl->sc_flags & SC_PACE)
        pacer_init(&ctl->sc_pacer, conn_pub->lconn,
        /* TODO: conn_pub has a pointer to enpub: drop third argument */
                                    enpub->enp_settings.es_clock_granularity);
    for (i = 0; i < sizeof(ctl->sc_buffered_packets) /
                                sizeof(ctl->sc_buffered_packets[0]); ++i)
        TAILQ_INIT(&ctl->sc_buffered_packets[i].bpq_packets);
    ctl->sc_max_packno_bits = PACKNO_BITS_2; /* Safe value before verneg */
    ctl->sc_cached_bpt.stream_id = UINT64_MAX;
}


static int
send_ctl_ecn_on (const struct lsquic_send_ctl *ctl)
{
    return ctl->sc_ecn != ECN_NOT_ECT;
}


static lsquic_time_t
calculate_packet_rto (lsquic_send_ctl_t *ctl)
{
    lsquic_time_t delay;

    delay = get_retx_delay(&ctl->sc_conn_pub->rtt_stats);

    unsigned exp = ctl->sc_n_consec_rtos;
    if (exp > MAX_RTO_BACKOFFS)
        exp = MAX_RTO_BACKOFFS;

    delay = delay * (1 << exp);

    return delay;
}


static lsquic_time_t
calculate_tlp_delay (lsquic_send_ctl_t *ctl)
{
    lsquic_time_t srtt, delay;

    srtt = lsquic_rtt_stats_get_srtt(&ctl->sc_conn_pub->rtt_stats);
    if (ctl->sc_n_in_flight_all > 1)
    {
        delay = 10000;  /* 10 ms is the minimum tail loss probe delay */
        if (delay < 2 * srtt)
            delay = 2 * srtt;
    }
    else
    {
        delay = srtt + srtt / 2 + MIN_RTO_DELAY;
        if (delay < 2 * srtt)
            delay = 2 * srtt;
    }

    return delay;
}


static void
set_retx_alarm (struct lsquic_send_ctl *ctl, enum packnum_space pns,
                                                            lsquic_time_t now)
{
    enum retx_mode rm;
    lsquic_time_t delay;

    assert(!TAILQ_EMPTY(&ctl->sc_unacked_packets[pns]));

    rm = get_retx_mode(ctl);
    switch (rm)
    {
    case RETX_MODE_HANDSHAKE:
    /* [draft-iyengar-quic-loss-recovery-01]:
     *
     *  if (handshake packets are outstanding):
     *      alarm_duration = max(1.5 * smoothed_rtt, 10ms) << handshake_count;
     *      handshake_count++;
     */
        delay = lsquic_rtt_stats_get_srtt(&ctl->sc_conn_pub->rtt_stats);
        if (delay)
        {
            delay += delay / 2;
            if (10000 > delay)
                delay = 10000;
        }
        else
            delay = 150000;
        delay <<= ctl->sc_n_hsk;
        ++ctl->sc_n_hsk;
        break;
    case RETX_MODE_LOSS:
        delay = ctl->sc_loss_to;
        break;
    case RETX_MODE_TLP:
        delay = calculate_tlp_delay(ctl);
        break;
    default:
        assert(rm == RETX_MODE_RTO);
        /* XXX the comment below as well as the name of the function
         * that follows seem obsolete.
         */
        /* Base RTO on the first unacked packet, following reference
         * implementation.
         */
        delay = calculate_packet_rto(ctl);
        break;
    }

    if (delay > MAX_RTO_DELAY)
        delay = MAX_RTO_DELAY;

    LSQ_DEBUG("set retx alarm to %"PRIu64", which is %"PRIu64
        " usec from now, mode %s", now + delay, delay, retx2str[rm]);
    lsquic_alarmset_set(ctl->sc_alset, AL_RETX_INIT + pns, now + delay);
}


static int
send_ctl_in_recovery (lsquic_send_ctl_t *ctl)
{
    return ctl->sc_largest_acked_packno
        && ctl->sc_largest_acked_packno <= ctl->sc_largest_sent_at_cutback;
}


#define SC_PACK_SIZE(ctl_) (+(ctl_)->sc_conn_pub->path->np_pack_size)

static lsquic_time_t
send_ctl_transfer_time (void *ctx)
{
    lsquic_send_ctl_t *const ctl = ctx;
    lsquic_time_t tx_time;
    uint64_t pacing_rate;
    int in_recovery;

    in_recovery = send_ctl_in_recovery(ctl);
    pacing_rate = ctl->sc_ci->cci_pacing_rate(CGP(ctl), in_recovery);
    tx_time = (uint64_t) SC_PACK_SIZE(ctl) * 1000000 / pacing_rate;
    return tx_time;
}


static void
send_ctl_unacked_append (struct lsquic_send_ctl *ctl,
                         struct lsquic_packet_out *packet_out)
{
    enum packnum_space pns;

    pns = lsquic_packet_out_pns(packet_out);
    assert(0 == (packet_out->po_flags & PO_LOSS_REC));
    TAILQ_INSERT_TAIL(&ctl->sc_unacked_packets[pns], packet_out, po_next);
    packet_out->po_flags |= PO_UNACKED;
    ctl->sc_bytes_unacked_all += packet_out_sent_sz(packet_out);
    ctl->sc_n_in_flight_all  += 1;
    if (packet_out->po_frame_types & ctl->sc_retx_frames)
    {
        ctl->sc_bytes_unacked_retx += packet_out_total_sz(packet_out);
        ++ctl->sc_n_in_flight_retx;
    }
}


static void
send_ctl_unacked_remove (struct lsquic_send_ctl *ctl,
                     struct lsquic_packet_out *packet_out, unsigned packet_sz)
{
    enum packnum_space pns;

    pns = lsquic_packet_out_pns(packet_out);
    TAILQ_REMOVE(&ctl->sc_unacked_packets[pns], packet_out, po_next);
    packet_out->po_flags &= ~PO_UNACKED;
    assert(ctl->sc_bytes_unacked_all >= packet_sz);
    ctl->sc_bytes_unacked_all -= packet_sz;
    ctl->sc_n_in_flight_all  -= 1;
    if (packet_out->po_frame_types & ctl->sc_retx_frames)
    {
        ctl->sc_bytes_unacked_retx -= packet_sz;
        --ctl->sc_n_in_flight_retx;
    }
}


static void
send_ctl_sched_Xpend_common (struct lsquic_send_ctl *ctl,
                      struct lsquic_packet_out *packet_out)
{
    packet_out->po_flags |= PO_SCHED;
    ++ctl->sc_n_scheduled;
    ctl->sc_bytes_scheduled += packet_out_total_sz(packet_out);
    lsquic_send_ctl_sanity_check(ctl);
}


static void
send_ctl_sched_append (struct lsquic_send_ctl *ctl,
                       struct lsquic_packet_out *packet_out)
{
    TAILQ_INSERT_TAIL(&ctl->sc_scheduled_packets, packet_out, po_next);
    send_ctl_sched_Xpend_common(ctl, packet_out);
}


static void
send_ctl_sched_prepend (struct lsquic_send_ctl *ctl,
                       struct lsquic_packet_out *packet_out)
{
    TAILQ_INSERT_HEAD(&ctl->sc_scheduled_packets, packet_out, po_next);
    send_ctl_sched_Xpend_common(ctl, packet_out);
}


static void
send_ctl_sched_remove (struct lsquic_send_ctl *ctl,
                       struct lsquic_packet_out *packet_out)
{
    TAILQ_REMOVE(&ctl->sc_scheduled_packets, packet_out, po_next);
    packet_out->po_flags &= ~PO_SCHED;
    assert(ctl->sc_n_scheduled);
    --ctl->sc_n_scheduled;
    ctl->sc_bytes_scheduled -= packet_out_total_sz(packet_out);
    lsquic_send_ctl_sanity_check(ctl);
}


int
lsquic_send_ctl_sent_packet (lsquic_send_ctl_t *ctl,
                             struct lsquic_packet_out *packet_out)
{
    enum packnum_space pns;
    char frames[lsquic_frame_types_str_sz];

    assert(!(packet_out->po_flags & PO_ENCRYPTED));
    ctl->sc_last_sent_time = packet_out->po_sent;
    pns = lsquic_packet_out_pns(packet_out);
    LSQ_DEBUG("packet %"PRIu64" has been sent (frame types: %s)",
        packet_out->po_packno, lsquic_frame_types_to_str(frames,
            sizeof(frames), packet_out->po_frame_types));
    lsquic_senhist_add(&ctl->sc_senhist, packet_out->po_packno);
    send_ctl_unacked_append(ctl, packet_out);
    if (packet_out->po_frame_types & ctl->sc_retx_frames)
    {
        if (!lsquic_alarmset_is_set(ctl->sc_alset, AL_RETX_INIT + pns))
            set_retx_alarm(ctl, pns, packet_out->po_sent);
        if (ctl->sc_n_in_flight_retx == 1)
            ctl->sc_flags |= SC_WAS_QUIET;
    }
    /* TODO: Do we really want to use those for RTT info? Revisit this. */
    /* Hold on to packets that are not retransmittable because we need them
     * to sample RTT information.  They are released when ACK is received.
     */
#if LSQUIC_SEND_STATS
    ++ctl->sc_stats.n_total_sent;
#endif
    if (ctl->sc_ci->cci_sent)
        ctl->sc_ci->cci_sent(CGP(ctl), packet_out, ctl->sc_n_in_flight_all,
                                            ctl->sc_flags & SC_APP_LIMITED);
    lsquic_send_ctl_sanity_check(ctl);
    return 0;
}


static void
take_rtt_sample (lsquic_send_ctl_t *ctl,
                 lsquic_time_t now, lsquic_time_t lack_delta)
{
    const lsquic_packno_t packno = ctl->sc_largest_acked_packno;
    const lsquic_time_t sent = ctl->sc_largest_acked_sent_time;
    const lsquic_time_t measured_rtt = now - sent;
    if (packno > ctl->sc_max_rtt_packno && lack_delta < measured_rtt)
    {
        ctl->sc_max_rtt_packno = packno;
        lsquic_rtt_stats_update(&ctl->sc_conn_pub->rtt_stats, measured_rtt, lack_delta);
        LSQ_DEBUG("packno %"PRIu64"; rtt: %"PRIu64"; delta: %"PRIu64"; "
            "new srtt: %"PRIu64, packno, measured_rtt, lack_delta,
            lsquic_rtt_stats_get_srtt(&ctl->sc_conn_pub->rtt_stats));
    }
}


static void
send_ctl_return_enc_data (struct lsquic_send_ctl *ctl,
                                        struct lsquic_packet_out *packet_out)
{
    ctl->sc_enpub->enp_pmi->pmi_return(ctl->sc_enpub->enp_pmi_ctx,
        packet_out->po_path->np_peer_ctx,
        packet_out->po_enc_data, lsquic_packet_out_ipv6(packet_out));
    packet_out->po_flags &= ~PO_ENCRYPTED;
    packet_out->po_enc_data = NULL;
}


static void
send_ctl_destroy_packet (struct lsquic_send_ctl *ctl,
                                        struct lsquic_packet_out *packet_out)
{
    if (0 == (packet_out->po_flags & PO_LOSS_REC))
        lsquic_packet_out_destroy(packet_out, ctl->sc_enpub,
                                            packet_out->po_path->np_peer_ctx);
    else
        lsquic_malo_put(packet_out);
}


static void
send_ctl_maybe_renumber_sched_to_right (struct lsquic_send_ctl *ctl,
                                        const struct lsquic_packet_out *cur)
{
    struct lsquic_packet_out *packet_out;

    /* If current packet has PO_REPACKNO set, it means that all those to the
     * right of it have this flag set as well.
     */
    if (0 == (cur->po_flags & PO_REPACKNO))
    {
        ctl->sc_cur_packno = cur->po_packno - 1;
        for (packet_out = TAILQ_NEXT(cur, po_next);
                packet_out && 0 == (packet_out->po_flags & PO_REPACKNO);
                    packet_out = TAILQ_NEXT(packet_out, po_next))
        {
            packet_out->po_flags |= PO_REPACKNO;
        }
    }
}


/* The third argument to advance `next' pointer when modifying the unacked
 * queue.  This is because the unacked queue may contain several elements
 * of the same chain.  This is not true of the lost and scheduled packet
 * queue, as the loss records are only present on the unacked queue.
 */
static void
send_ctl_destroy_chain (struct lsquic_send_ctl *ctl,
                        struct lsquic_packet_out *const packet_out,
                        struct lsquic_packet_out **next)
{
    struct lsquic_packet_out *chain_cur, *chain_next;
    unsigned packet_sz, count;
    enum packnum_space pns = lsquic_packet_out_pns(packet_out);

    count = 0;
    for (chain_cur = packet_out->po_loss_chain; chain_cur != packet_out;
                                                    chain_cur = chain_next)
    {
        chain_next = chain_cur->po_loss_chain;
        switch (chain_cur->po_flags & (PO_SCHED|PO_UNACKED|PO_LOST))
        {
        case PO_SCHED:
            send_ctl_maybe_renumber_sched_to_right(ctl, chain_cur);
            send_ctl_sched_remove(ctl, chain_cur);
            break;
        case PO_UNACKED:
            if (chain_cur->po_flags & PO_LOSS_REC)
                TAILQ_REMOVE(&ctl->sc_unacked_packets[pns], chain_cur, po_next);
            else
            {
                packet_sz = packet_out_sent_sz(chain_cur);
                send_ctl_unacked_remove(ctl, chain_cur, packet_sz);
            }
            break;
        case PO_LOST:
            TAILQ_REMOVE(&ctl->sc_lost_packets, chain_cur, po_next);
            break;
        case 0:
            /* This is also weird, but let it pass */
            break;
        default:
            assert(0);
            break;
        }
        if (next && *next == chain_cur)
            *next = TAILQ_NEXT(*next, po_next);
        if (0 == (chain_cur->po_flags & PO_LOSS_REC))
            lsquic_packet_out_ack_streams(chain_cur);
        send_ctl_destroy_packet(ctl, chain_cur);
        ++count;
    }
    packet_out->po_loss_chain = packet_out;

    if (count)
        LSQ_DEBUG("destroyed %u packet%.*s in chain of packet %"PRIu64,
            count, count != 1, "s", packet_out->po_packno);
}


static void
send_ctl_record_loss (struct lsquic_send_ctl *ctl,
                                        struct lsquic_packet_out *packet_out)
{
    struct lsquic_packet_out *loss_record;

    loss_record = lsquic_malo_get(ctl->sc_conn_pub->packet_out_malo);
    if (loss_record)
    {
        memset(loss_record, 0, sizeof(*loss_record));
        loss_record->po_flags = PO_UNACKED|PO_LOSS_REC|PO_SENT_SZ;
        loss_record->po_flags |=
            ((packet_out->po_flags >> POPNS_SHIFT) & 3) << POPNS_SHIFT;
        /* Copy values used in ACK processing: */
        loss_record->po_packno = packet_out->po_packno;
        loss_record->po_sent = packet_out->po_sent;
        loss_record->po_sent_sz = packet_out_sent_sz(packet_out);
        loss_record->po_frame_types = packet_out->po_frame_types;
        /* Insert the loss record into the chain: */
        loss_record->po_loss_chain = packet_out->po_loss_chain;
        packet_out->po_loss_chain = loss_record;
        /* Place the loss record next to the lost packet we are about to
         * remove from the list:
         */
        TAILQ_INSERT_BEFORE(packet_out, loss_record, po_next);
    }
    else
        LSQ_INFO("cannot allocate memory for loss record");
}


/* Returns true if packet was rescheduled, false otherwise.  In the latter
 * case, you should not dereference packet_out after the function returns.
 */
static int
send_ctl_handle_lost_packet (lsquic_send_ctl_t *ctl,
            lsquic_packet_out_t *packet_out, struct lsquic_packet_out **next)
{
    unsigned packet_sz;

    assert(ctl->sc_n_in_flight_all);
    packet_sz = packet_out_sent_sz(packet_out);

    ++ctl->sc_loss_count;

    if (packet_out->po_frame_types & (1 << QUIC_FRAME_ACK))
    {
        ctl->sc_flags |= SC_LOST_ACK_INIT << lsquic_packet_out_pns(packet_out);
        LSQ_DEBUG("lost ACK in packet %"PRIu64, packet_out->po_packno);
    }

    if (ctl->sc_ci->cci_lost)
        ctl->sc_ci->cci_lost(CGP(ctl), packet_out, packet_sz);

    /* This is a client-only check, server check happens in mini conn */
    if (send_ctl_ecn_on(ctl)
            && 0 == ctl->sc_ecn_total_acked[PNS_INIT]
                && HETY_INITIAL == packet_out->po_header_type
                    && 3 == packet_out->po_packno)
    {
        LSQ_DEBUG("possible ECN black hole during handshake, disable ECN");
        ctl->sc_ecn = ECN_NOT_ECT;
    }

    if (packet_out->po_frame_types & ctl->sc_retx_frames)
    {
        LSQ_DEBUG("lost retransmittable packet %"PRIu64,
                                                    packet_out->po_packno);
        send_ctl_record_loss(ctl, packet_out);
        send_ctl_unacked_remove(ctl, packet_out, packet_sz);
        TAILQ_INSERT_TAIL(&ctl->sc_lost_packets, packet_out, po_next);
        packet_out->po_flags |= PO_LOST;
        return 1;
    }
    else
    {
        LSQ_DEBUG("lost unretransmittable packet %"PRIu64,
                                                    packet_out->po_packno);
        send_ctl_unacked_remove(ctl, packet_out, packet_sz);
        send_ctl_destroy_chain(ctl, packet_out, next);
        send_ctl_destroy_packet(ctl, packet_out);
        return 0;
    }
}


static lsquic_packno_t
largest_retx_packet_number (const struct lsquic_send_ctl *ctl,
                                                    enum packnum_space pns)
{
    const lsquic_packet_out_t *packet_out;
    TAILQ_FOREACH_REVERSE(packet_out, &ctl->sc_unacked_packets[pns],
                                                lsquic_packets_tailq, po_next)
    {
        if (0 == (packet_out->po_flags & PO_LOSS_REC)
                && (packet_out->po_frame_types & ctl->sc_retx_frames))
            return packet_out->po_packno;
    }
    return 0;
}


static void
send_ctl_detect_losses (struct lsquic_send_ctl *ctl, enum packnum_space pns,
                                                            lsquic_time_t time)
{
    lsquic_packet_out_t *packet_out, *next;
    lsquic_packno_t largest_retx_packno, largest_lost_packno;

    largest_retx_packno = largest_retx_packet_number(ctl, pns);
    largest_lost_packno = 0;
    ctl->sc_loss_to = 0;

    for (packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns]);
            packet_out && packet_out->po_packno <= ctl->sc_largest_acked_packno;
                packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);

        if (packet_out->po_flags & PO_LOSS_REC)
            continue;

        if (packet_out->po_packno + N_NACKS_BEFORE_RETX <
                                                ctl->sc_largest_acked_packno)
        {
            LSQ_DEBUG("loss by FACK detected, packet %"PRIu64,
                                                    packet_out->po_packno);
            largest_lost_packno = packet_out->po_packno;
            (void) send_ctl_handle_lost_packet(ctl, packet_out, &next);
            continue;
        }

        if (largest_retx_packno
            && (packet_out->po_frame_types & ctl->sc_retx_frames)
            && largest_retx_packno <= ctl->sc_largest_acked_packno)
        {
            LSQ_DEBUG("loss by early retransmit detected, packet %"PRIu64,
                                                    packet_out->po_packno);
            largest_lost_packno = packet_out->po_packno;
            ctl->sc_loss_to =
                lsquic_rtt_stats_get_srtt(&ctl->sc_conn_pub->rtt_stats) / 4;
            LSQ_DEBUG("set sc_loss_to to %"PRIu64", packet %"PRIu64,
                                    ctl->sc_loss_to, packet_out->po_packno);
            (void) send_ctl_handle_lost_packet(ctl, packet_out, &next);
            continue;
        }

        if (ctl->sc_largest_acked_sent_time > packet_out->po_sent +
                    lsquic_rtt_stats_get_srtt(&ctl->sc_conn_pub->rtt_stats))
        {
            LSQ_DEBUG("loss by sent time detected: packet %"PRIu64,
                                                    packet_out->po_packno);
            if (packet_out->po_frame_types & ctl->sc_retx_frames)
                largest_lost_packno = packet_out->po_packno;
            else { /* don't count it as a loss */; }
            (void) send_ctl_handle_lost_packet(ctl, packet_out, &next);
            continue;
        }
    }

    if (largest_lost_packno > ctl->sc_largest_sent_at_cutback)
    {
        LSQ_DEBUG("detected new loss: packet %"PRIu64"; new lsac: "
            "%"PRIu64, largest_lost_packno, ctl->sc_largest_sent_at_cutback);
        ctl->sc_ci->cci_loss(CGP(ctl));
        if (ctl->sc_flags & SC_PACE)
            pacer_loss_event(&ctl->sc_pacer);
        ctl->sc_largest_sent_at_cutback =
                                lsquic_senhist_largest(&ctl->sc_senhist);
    }
    else if (largest_lost_packno)
        /* Lost packets whose numbers are smaller than the largest packet
         * number sent at the time of the last loss event indicate the same
         * loss event.  This follows NewReno logic, see RFC 6582.
         */
        LSQ_DEBUG("ignore loss of packet %"PRIu64" smaller than lsac "
            "%"PRIu64, largest_lost_packno, ctl->sc_largest_sent_at_cutback);
}


int
lsquic_send_ctl_got_ack (lsquic_send_ctl_t *ctl,
                         const struct ack_info *acki,
                         lsquic_time_t ack_recv_time, lsquic_time_t now)
{
    const struct lsquic_packno_range *range =
                                    &acki->ranges[ acki->n_ranges - 1 ];
    lsquic_packet_out_t *packet_out, *next;
    lsquic_packno_t smallest_unacked;
    lsquic_packno_t ack2ed[2];
    unsigned packet_sz;
    int app_limited;
    signed char do_rtt, skip_checks;
    enum packnum_space pns;
    unsigned ecn_total_acked, ecn_ce_cnt, one_rtt_cnt;

    pns = acki->pns;
    packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns]);
#if __GNUC__
    __builtin_prefetch(packet_out);
#endif

#if __GNUC__
#   define UNLIKELY(cond) __builtin_expect(cond, 0)
#else
#   define UNLIKELY(cond) cond
#endif

#if __GNUC__
    if (UNLIKELY(LSQ_LOG_ENABLED(LSQ_LOG_DEBUG)))
#endif
        LSQ_DEBUG("Got ACK frame, largest acked: %"PRIu64"; delta: %"PRIu64,
                            largest_acked(acki), acki->lack_delta);

    /* Validate ACK first: */
    if (UNLIKELY(largest_acked(acki)
                                > lsquic_senhist_largest(&ctl->sc_senhist)))
    {
        LSQ_INFO("at least one packet in ACK range [%"PRIu64" - %"PRIu64"] "
            "was never sent", acki->ranges[0].low, acki->ranges[0].high);
        return -1;
    }

    if (ctl->sc_ci->cci_begin_ack)
        ctl->sc_ci->cci_begin_ack(CGP(ctl), ack_recv_time,
                                                    ctl->sc_bytes_unacked_all);

    ecn_total_acked = 0;
    ecn_ce_cnt = 0;
    one_rtt_cnt = 0;

    if (UNLIKELY(ctl->sc_flags & SC_WAS_QUIET))
    {
        ctl->sc_flags &= ~SC_WAS_QUIET;
        LSQ_DEBUG("ACK comes after a period of quiescence");
        ctl->sc_ci->cci_was_quiet(CGP(ctl), now, ctl->sc_bytes_unacked_all);
    }

    if (UNLIKELY(!packet_out))
        goto no_unacked_packets;

    smallest_unacked = packet_out->po_packno;
    LSQ_DEBUG("Smallest unacked: %"PRIu64, smallest_unacked);

    ack2ed[1] = 0;

    if (packet_out->po_packno > largest_acked(acki))
        goto detect_losses;

    if (largest_acked(acki) > ctl->sc_cur_rt_end)
    {
        ++ctl->sc_rt_count;
        ctl->sc_cur_rt_end = lsquic_senhist_largest(&ctl->sc_senhist);
    }

    do_rtt = 0, skip_checks = 0;
    app_limited = -1;
    do
    {
        next = TAILQ_NEXT(packet_out, po_next);
#if __GNUC__
        __builtin_prefetch(next);
#endif
        if (skip_checks)
            goto after_checks;
        /* This is faster than binary search in the normal case when the number
         * of ranges is not much larger than the number of unacked packets.
         */
        while (UNLIKELY(range->high < packet_out->po_packno))
            --range;
        if (range->low <= packet_out->po_packno)
        {
            skip_checks = range == acki->ranges;
            if (app_limited < 0)
                app_limited = send_ctl_retx_bytes_out(ctl) + 3 * SC_PACK_SIZE(ctl) /* This
                    is the "maximum burst" parameter */
                    < ctl->sc_ci->cci_get_cwnd(CGP(ctl));
  after_checks:
            ctl->sc_largest_acked_packno    = packet_out->po_packno;
            ctl->sc_largest_acked_sent_time = packet_out->po_sent;
            ecn_total_acked += lsquic_packet_out_ecn(packet_out) != ECN_NOT_ECT;
            ecn_ce_cnt += lsquic_packet_out_ecn(packet_out) == ECN_CE;
            one_rtt_cnt += lsquic_packet_out_enc_level(packet_out) == ENC_LEV_FORW;
            if (0 == (packet_out->po_flags & PO_LOSS_REC))
            {
                packet_sz = packet_out_sent_sz(packet_out);
                send_ctl_unacked_remove(ctl, packet_out, packet_sz);
                lsquic_packet_out_ack_streams(packet_out);
                LSQ_DEBUG("acking via regular record %"PRIu64,
                                                        packet_out->po_packno);
            }
            else
            {
                packet_sz = packet_out->po_sent_sz;
                TAILQ_REMOVE(&ctl->sc_unacked_packets[pns], packet_out,
                                                                    po_next);
                LSQ_DEBUG("acking via loss record %"PRIu64,
                                                        packet_out->po_packno);
#if LSQUIC_CONN_STATS
                ++ctl->sc_conn_pub->conn_stats->out.acked_via_loss;
                LSQ_DEBUG("acking via loss record %"PRIu64,
                                                        packet_out->po_packno);
#endif
            }
            ack2ed[!!(packet_out->po_frame_types & (1 << QUIC_FRAME_ACK))]
                = packet_out->po_ack2ed;
            do_rtt |= packet_out->po_packno == largest_acked(acki);
            ctl->sc_ci->cci_ack(CGP(ctl), packet_out, packet_sz, now,
                                                             app_limited);
            send_ctl_destroy_chain(ctl, packet_out, &next);
            send_ctl_destroy_packet(ctl, packet_out);
        }
        packet_out = next;
    }
    while (packet_out && packet_out->po_packno <= largest_acked(acki));

    if (do_rtt)
    {
        take_rtt_sample(ctl, ack_recv_time, acki->lack_delta);
        ctl->sc_n_consec_rtos = 0;
        ctl->sc_n_hsk = 0;
        ctl->sc_n_tlp = 0;
    }

  detect_losses:
    send_ctl_detect_losses(ctl, pns, ack_recv_time);
    if (send_ctl_first_unacked_retx_packet(ctl, pns))
        set_retx_alarm(ctl, pns, now);
    else
    {
        LSQ_DEBUG("No retransmittable packets: clear alarm");
        lsquic_alarmset_unset(ctl->sc_alset, AL_RETX_INIT + pns);
    }
    lsquic_send_ctl_sanity_check(ctl);

    if ((ctl->sc_flags & SC_NSTP) && ack2ed[1] > ctl->sc_largest_ack2ed[pns])
        ctl->sc_largest_ack2ed[pns] = ack2ed[1];

    if (ctl->sc_n_in_flight_retx == 0)
        ctl->sc_flags |= SC_WAS_QUIET;

    if (one_rtt_cnt)
        ctl->sc_flags |= SC_1RTT_ACKED;

    if (send_ctl_ecn_on(ctl))
    {
        const uint64_t sum = acki->ecn_counts[ECN_ECT0]
                           + acki->ecn_counts[ECN_ECT1]
                           + acki->ecn_counts[ECN_CE];
        ctl->sc_ecn_total_acked[pns] += ecn_total_acked;
        ctl->sc_ecn_ce_cnt[pns] += ecn_ce_cnt;
        if (sum >= ctl->sc_ecn_total_acked[pns])
        {
            if (sum > ctl->sc_ecn_total_acked[pns])
                ctl->sc_ecn_total_acked[pns] = sum;
            if (acki->ecn_counts[ECN_CE] > ctl->sc_ecn_ce_cnt[pns])
            {
                ctl->sc_ecn_ce_cnt[pns] = acki->ecn_counts[ECN_CE];
                LSQ_WARN("TODO: handle ECN CE event");  /* XXX TODO */
            }
        }
        else
        {
            LSQ_INFO("ECN total ACKed (%"PRIu64") is greater than the sum "
                "of ECN counters (%"PRIu64"): disable ECN",
                ctl->sc_ecn_total_acked[pns], sum);
            ctl->sc_ecn = ECN_NOT_ECT;
        }
    }

  update_n_stop_waiting:
    if (!(ctl->sc_flags & (SC_NSTP|SC_IETF)))
    {
        if (smallest_unacked > smallest_acked(acki))
            /* Peer is acking packets that have been acked already.  Schedule
             * ACK and STOP_WAITING frame to chop the range if we get two of
             * these in a row.
             */
            ++ctl->sc_n_stop_waiting;
        else
            ctl->sc_n_stop_waiting = 0;
    }
    lsquic_send_ctl_sanity_check(ctl);
    if (ctl->sc_ci->cci_end_ack)
        ctl->sc_ci->cci_end_ack(CGP(ctl), ctl->sc_bytes_unacked_all);
    return 0;

  no_unacked_packets:
    smallest_unacked = lsquic_senhist_largest(&ctl->sc_senhist) + 1;
    ctl->sc_flags |= SC_WAS_QUIET;
    goto update_n_stop_waiting;
}


lsquic_packno_t
lsquic_send_ctl_smallest_unacked (lsquic_send_ctl_t *ctl)
{
    const lsquic_packet_out_t *packet_out;
    enum packnum_space pns;

    /* Packets are always sent out in order (unless we are reordering them
     * on purpose).  Thus, the first packet on the unacked packets list has
     * the smallest packet number of all packets on that list.
     */
    for (pns = ctl->sc_flags & SC_IETF ? PNS_INIT : PNS_APP; pns < N_PNS; ++pns)
        if ((packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns])))
            /* We're OK with using a loss record */
            return packet_out->po_packno;

    return lsquic_senhist_largest(&ctl->sc_senhist) + first_packno(ctl);
}


static struct lsquic_packet_out *
send_ctl_next_lost (lsquic_send_ctl_t *ctl)
{
    struct lsquic_packet_out *lost_packet;

  get_next_lost:
    lost_packet = TAILQ_FIRST(&ctl->sc_lost_packets);
    if (lost_packet)
    {
        if (lost_packet->po_frame_types & (1 << QUIC_FRAME_STREAM))
        {
            if (0 == (lost_packet->po_flags & PO_MINI))
            {
                lsquic_packet_out_elide_reset_stream_frames(lost_packet, 0);
                if (lost_packet->po_regen_sz >= lost_packet->po_data_sz)
                {
                    LSQ_DEBUG("Dropping packet %"PRIu64" from lost queue",
                        lost_packet->po_packno);
                    TAILQ_REMOVE(&ctl->sc_lost_packets, lost_packet, po_next);
                    lost_packet->po_flags &= ~PO_LOST;
                    send_ctl_destroy_chain(ctl, lost_packet, NULL);
                    send_ctl_destroy_packet(ctl, lost_packet);
                    goto get_next_lost;
                }
            }
            else
            {
                /* Mini connection only ever sends data on stream 1.  There
                 * is nothing to elide: always resend it.
                 */
                ;
            }
        }

        if (!lsquic_send_ctl_can_send(ctl))
            return NULL;

        TAILQ_REMOVE(&ctl->sc_lost_packets, lost_packet, po_next);
        lost_packet->po_flags &= ~PO_LOST;
        lost_packet->po_flags |= PO_RETX;
    }

    return lost_packet;
}


static lsquic_packno_t
send_ctl_next_packno (lsquic_send_ctl_t *ctl)
{
    return ++ctl->sc_cur_packno;
}


void
lsquic_send_ctl_cleanup (lsquic_send_ctl_t *ctl)
{
    lsquic_packet_out_t *packet_out, *next;
    enum packnum_space pns;
    unsigned n;

    lsquic_senhist_cleanup(&ctl->sc_senhist);
    while ((packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets)))
    {
        send_ctl_sched_remove(ctl, packet_out);
        send_ctl_destroy_packet(ctl, packet_out);
    }
    assert(0 == ctl->sc_n_scheduled);
    assert(0 == ctl->sc_bytes_scheduled);
    for (pns = PNS_INIT; pns < N_PNS; ++pns)
        while ((packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns])))
        {
            TAILQ_REMOVE(&ctl->sc_unacked_packets[pns], packet_out, po_next);
            packet_out->po_flags &= ~PO_UNACKED;
#ifndef NDEBUG
            if (0 == (packet_out->po_flags & PO_LOSS_REC))
            {
                ctl->sc_bytes_unacked_all -= packet_out_sent_sz(packet_out);
                --ctl->sc_n_in_flight_all;
            }
#endif
            send_ctl_destroy_packet(ctl, packet_out);
        }
    assert(0 == ctl->sc_n_in_flight_all);
    assert(0 == ctl->sc_bytes_unacked_all);
    while ((packet_out = TAILQ_FIRST(&ctl->sc_lost_packets)))
    {
        TAILQ_REMOVE(&ctl->sc_lost_packets, packet_out, po_next);
        packet_out->po_flags &= ~PO_LOST;
        send_ctl_destroy_packet(ctl, packet_out);
    }
    for (n = 0; n < sizeof(ctl->sc_buffered_packets) /
                                sizeof(ctl->sc_buffered_packets[0]); ++n)
    {
        for (packet_out = TAILQ_FIRST(&ctl->sc_buffered_packets[n].bpq_packets);
                                                packet_out; packet_out = next)
        {
            next = TAILQ_NEXT(packet_out, po_next);
            send_ctl_destroy_packet(ctl, packet_out);
        }
    }
    if (ctl->sc_flags & SC_PACE)
        pacer_cleanup(&ctl->sc_pacer);
    ctl->sc_ci->cci_cleanup(CGP(ctl));
#if LSQUIC_SEND_STATS
    LSQ_NOTICE("stats: n_total_sent: %u; n_resent: %u; n_delayed: %u",
        ctl->sc_stats.n_total_sent, ctl->sc_stats.n_resent,
        ctl->sc_stats.n_delayed);
#endif
    free(ctl->sc_token);
}


static unsigned
send_ctl_retx_bytes_out (const struct lsquic_send_ctl *ctl)
{
    return ctl->sc_bytes_scheduled
         + ctl->sc_bytes_unacked_retx
         ;
}


static unsigned
send_ctl_all_bytes_out (const struct lsquic_send_ctl *ctl)
{
    return ctl->sc_bytes_scheduled
         + ctl->sc_bytes_unacked_all
         ;
}


int
lsquic_send_ctl_pacer_blocked (struct lsquic_send_ctl *ctl)
{
    return (ctl->sc_flags & SC_PACE)
        && !pacer_can_schedule(&ctl->sc_pacer,
                               ctl->sc_n_scheduled + ctl->sc_n_in_flight_all);
}


#ifndef NDEBUG
#if __GNUC__
__attribute__((weak))
#endif
#endif
int
lsquic_send_ctl_can_send (lsquic_send_ctl_t *ctl)
{
    const unsigned n_out = send_ctl_all_bytes_out(ctl);
    LSQ_DEBUG("%s: n_out: %u (unacked_all: %u); cwnd: %"PRIu64, __func__,
        n_out, ctl->sc_bytes_unacked_all,
        ctl->sc_ci->cci_get_cwnd(CGP(ctl)));
    if (ctl->sc_flags & SC_PACE)
    {
        if (n_out >= ctl->sc_ci->cci_get_cwnd(CGP(ctl)))
            return 0;
        if (pacer_can_schedule(&ctl->sc_pacer,
                               ctl->sc_n_scheduled + ctl->sc_n_in_flight_all))
            return 1;
        if (ctl->sc_flags & SC_SCHED_TICK)
        {
            ctl->sc_flags &= ~SC_SCHED_TICK;
            lsquic_engine_add_conn_to_attq(ctl->sc_enpub,
                    ctl->sc_conn_pub->lconn, pacer_next_sched(&ctl->sc_pacer),
                    AEW_PACER);
        }
        return 0;
    }
    else
        return n_out < ctl->sc_ci->cci_get_cwnd(CGP(ctl));
}


/* Like lsquic_send_ctl_can_send(), but no mods */
static int
send_ctl_could_send (const struct lsquic_send_ctl *ctl)
{
    uint64_t cwnd;
    unsigned n_out;

    if ((ctl->sc_flags & SC_PACE) && pacer_delayed(&ctl->sc_pacer))
        return 0;

    cwnd = ctl->sc_ci->cci_get_cwnd(CGP(ctl));
    n_out = send_ctl_all_bytes_out(ctl);
    return n_out < cwnd;
}


void
lsquic_send_ctl_maybe_app_limited (struct lsquic_send_ctl *ctl,
                                            const struct network_path *path)
{
    const struct lsquic_packet_out *packet_out;

    packet_out = lsquic_send_ctl_last_scheduled(ctl, PNS_APP, path, 0);
    if ((packet_out && lsquic_packet_out_avail(packet_out) > 10)
                                                || send_ctl_could_send(ctl))
    {
        LSQ_DEBUG("app-limited");
        ctl->sc_flags |= SC_APP_LIMITED;
    }
}


static void
send_ctl_expire (struct lsquic_send_ctl *ctl, enum packnum_space pns,
                                                    enum expire_filter filter)
{
    lsquic_packet_out_t *packet_out, *next;
    int n_resubmitted;
    static const char *const filter_type2str[] = {
        [EXFI_ALL] = "all",
        [EXFI_HSK] = "handshake",
        [EXFI_LAST] = "last",
    };

    switch (filter)
    {
    case EXFI_ALL:
        n_resubmitted = 0;
        for (packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns]);
                                                packet_out; packet_out = next)
        {
            next = TAILQ_NEXT(packet_out, po_next);
            if (0 == (packet_out->po_flags & PO_LOSS_REC))
                n_resubmitted += send_ctl_handle_lost_packet(ctl, packet_out,
                                                                        &next);
        }
        break;
    case EXFI_HSK:
        n_resubmitted = 0;
        for (packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns]); packet_out;
                                                            packet_out = next)
        {
            next = TAILQ_NEXT(packet_out, po_next);
            if (packet_out->po_flags & PO_HELLO)
                n_resubmitted += send_ctl_handle_lost_packet(ctl, packet_out,
                                                                        &next);
        }
        break;
    default:
        assert(filter == EXFI_LAST);
        packet_out = send_ctl_last_unacked_retx_packet(ctl, pns);
        if (packet_out)
            n_resubmitted = send_ctl_handle_lost_packet(ctl, packet_out, NULL);
        else
            n_resubmitted = 0;
        break;
    }

    LSQ_DEBUG("consider %s packets lost: %d resubmitted",
                                    filter_type2str[filter], n_resubmitted);
}


void
lsquic_send_ctl_expire_all (lsquic_send_ctl_t *ctl)
{
    enum packnum_space pns;

    for (pns = ctl->sc_flags & SC_IETF ? PNS_INIT : PNS_APP; pns < N_PNS; ++pns)
    {
        lsquic_alarmset_unset(ctl->sc_alset, AL_RETX_INIT + pns);
        send_ctl_expire(ctl, pns, EXFI_ALL);
    }
    lsquic_send_ctl_sanity_check(ctl);
}


#if LSQUIC_EXTRA_CHECKS
void
lsquic_send_ctl_sanity_check (const lsquic_send_ctl_t *ctl)
{
    const struct lsquic_packet_out *packet_out;
    lsquic_packno_t prev_packno;
    int prev_packno_set;
    unsigned count, bytes;
    enum packnum_space pns;

    assert(!send_ctl_first_unacked_retx_packet(ctl, PNS_APP) ||
                    lsquic_alarmset_is_set(ctl->sc_alset, AL_RETX_APP));
    if (lsquic_alarmset_is_set(ctl->sc_alset, AL_RETX_APP))
    {
        assert(send_ctl_first_unacked_retx_packet(ctl, PNS_APP));
        assert(lsquic_time_now()
                    < ctl->sc_alset->as_expiry[AL_RETX_APP] + MAX_RTO_DELAY);
    }

    count = 0, bytes = 0;
    for (pns = PNS_INIT; pns <= PNS_APP; ++pns)
    {
        prev_packno_set = 0;
        TAILQ_FOREACH(packet_out, &ctl->sc_unacked_packets[pns], po_next)
        {
            if (prev_packno_set)
                assert(packet_out->po_packno > prev_packno);
            else
            {
                prev_packno = packet_out->po_packno;
                prev_packno_set = 1;
            }
            if (0 == (packet_out->po_flags & PO_LOSS_REC))
            {
                bytes += packet_out_sent_sz(packet_out);
                ++count;
            }
        }
    }
    assert(count == ctl->sc_n_in_flight_all);
    assert(bytes == ctl->sc_bytes_unacked_all);

    count = 0, bytes = 0;
    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
    {
        assert(packet_out->po_flags & PO_SCHED);
        bytes += packet_out_total_sz(packet_out);
        ++count;
    }
    assert(count == ctl->sc_n_scheduled);
    assert(bytes == ctl->sc_bytes_scheduled);
}
#endif


void
lsquic_send_ctl_scheduled_one (lsquic_send_ctl_t *ctl,
                                            lsquic_packet_out_t *packet_out)
{
#ifndef NDEBUG
    const lsquic_packet_out_t *last;
    last = TAILQ_LAST(&ctl->sc_scheduled_packets, lsquic_packets_tailq);
    if (last)
        assert((last->po_flags & PO_REPACKNO) ||
                last->po_packno < packet_out->po_packno);
#endif
    if (ctl->sc_flags & SC_PACE)
    {
        unsigned n_out = ctl->sc_n_in_flight_retx + ctl->sc_n_scheduled;
        pacer_packet_scheduled(&ctl->sc_pacer, n_out,
            send_ctl_in_recovery(ctl), send_ctl_transfer_time, ctl);
    }
    send_ctl_sched_append(ctl, packet_out);
}


/* Wrapper is used to reset the counter when it's been too long */
static unsigned
send_ctl_get_n_consec_rtos (struct lsquic_send_ctl *ctl)
{
    lsquic_time_t timeout;

    if (ctl->sc_n_consec_rtos)
    {
        timeout = calculate_packet_rto(ctl);
        if (ctl->sc_last_rto_time + timeout < ctl->sc_last_sent_time)
        {
            ctl->sc_n_consec_rtos = 0;
            LSQ_DEBUG("reset RTO counter after %"PRIu64" usec",
                ctl->sc_last_sent_time - ctl->sc_last_rto_time);
        }
    }

    return ctl->sc_n_consec_rtos;
}


/* This mimics the logic in lsquic_send_ctl_next_packet_to_send(): we want
 * to check whether the first scheduled packet cannot be sent.
 */
int
lsquic_send_ctl_sched_is_blocked (struct lsquic_send_ctl *ctl)
{
    const lsquic_packet_out_t *packet_out
                            = TAILQ_FIRST(&ctl->sc_scheduled_packets);
    return send_ctl_get_n_consec_rtos(ctl)
        && 0 == ctl->sc_next_limit
        && packet_out
        && !(packet_out->po_frame_types & (1 << QUIC_FRAME_ACK));
}


static void
send_ctl_maybe_zero_pad (struct lsquic_send_ctl *ctl,
                        struct lsquic_packet_out *initial_packet, size_t limit)
{
    struct lsquic_packet_out *packet_out;
    size_t cum_size, size;

    cum_size = packet_out_total_sz(initial_packet);
    if (cum_size >= limit)
        return;

    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
    {
        size = packet_out_total_sz(packet_out);
        if (cum_size + size > SC_PACK_SIZE(ctl))
            break;
        cum_size += size;
        if (cum_size >= limit)
            return;
    }

    assert(cum_size < limit);
    size = limit - cum_size;
    if (size > lsquic_packet_out_avail(initial_packet))
        size = lsquic_packet_out_avail(initial_packet);
    memset(initial_packet->po_data + initial_packet->po_data_sz, 0, size);
    initial_packet->po_data_sz += size;
    initial_packet->po_frame_types |= QUIC_FTBIT_PADDING;
    LSQ_DEBUG("Added %zu bytes of PADDING to packet %"PRIu64, size,
                                                initial_packet->po_packno);
}


lsquic_packet_out_t *
lsquic_send_ctl_next_packet_to_send (struct lsquic_send_ctl *ctl, size_t size)
{
    lsquic_packet_out_t *packet_out;
    int dec_limit;

  get_packet:
    packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets);
    if (!packet_out)
        return NULL;

    if (!(packet_out->po_frame_types & (1 << QUIC_FRAME_ACK))
                                        && send_ctl_get_n_consec_rtos(ctl))
    {
        if (ctl->sc_next_limit)
            dec_limit = 1;
        else
            return NULL;
    }
    else
        dec_limit = 0;

    if (packet_out->po_flags & PO_REPACKNO)
    {
        if (packet_out->po_regen_sz < packet_out->po_data_sz)
        {
            update_for_resending(ctl, packet_out);
            packet_out->po_flags &= ~PO_REPACKNO;
        }
        else
        {
            LSQ_DEBUG("Dropping packet %"PRIu64" from scheduled queue",
                packet_out->po_packno);
            send_ctl_sched_remove(ctl, packet_out);
            send_ctl_destroy_chain(ctl, packet_out, NULL);
            send_ctl_destroy_packet(ctl, packet_out);
            goto get_packet;
        }
    }

    if (UNLIKELY(size))
    {
        if (packet_out_total_sz(packet_out) + size > SC_PACK_SIZE(ctl))
            return NULL;
        LSQ_DEBUG("packet %"PRIu64" will be tacked on to previous packet "
                                    "(coalescing)", packet_out->po_packno);
    }
    send_ctl_sched_remove(ctl, packet_out);

    if (dec_limit)
    {
        --ctl->sc_next_limit;
        packet_out->po_flags |= PO_LIMITED;
    }
    else
        packet_out->po_flags &= ~PO_LIMITED;

    if (UNLIKELY(packet_out->po_header_type == HETY_INITIAL)
                    && !(ctl->sc_conn_pub->lconn->cn_flags & LSCONN_SERVER))
    {
        send_ctl_maybe_zero_pad(ctl, packet_out, size ? size : 1200);
    }

    if (ctl->sc_flags & SC_QL_BITS)
    {
        packet_out->po_lflags |= POL_LOG_QL_BITS;
        if (ctl->sc_loss_count)
        {
            --ctl->sc_loss_count;
            packet_out->po_lflags |= POL_LOSS_BIT;
        }
        else
            packet_out->po_lflags &= ~POL_LOSS_BIT;
        if (packet_out->po_header_type == HETY_NOT_SET)
        {
            if (ctl->sc_square_count++ & 128)
                packet_out->po_lflags |= POL_SQUARE_BIT;
            else
                packet_out->po_lflags &= ~POL_SQUARE_BIT;
        }
    }

    return packet_out;
}


void
lsquic_send_ctl_delayed_one (lsquic_send_ctl_t *ctl,
                                            lsquic_packet_out_t *packet_out)
{
    send_ctl_sched_prepend(ctl, packet_out);
    if (packet_out->po_flags & PO_LIMITED)
        ++ctl->sc_next_limit;
    LSQ_DEBUG("packet %"PRIu64" has been delayed", packet_out->po_packno);
#if LSQUIC_SEND_STATS
    ++ctl->sc_stats.n_delayed;
#endif
    if (packet_out->po_lflags & POL_LOSS_BIT)
        ++ctl->sc_loss_count;
}


int
lsquic_send_ctl_have_outgoing_stream_frames (const lsquic_send_ctl_t *ctl)
{
    const lsquic_packet_out_t *packet_out;
    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        if (packet_out->po_frame_types &
                    ((1 << QUIC_FRAME_STREAM) | (1 << QUIC_FRAME_RST_STREAM)))
            return 1;
    return 0;
}


int
lsquic_send_ctl_have_outgoing_retx_frames (const lsquic_send_ctl_t *ctl)
{
    const lsquic_packet_out_t *packet_out;
    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        if (packet_out->po_frame_types & ctl->sc_retx_frames)
            return 1;
    return 0;
}


static int
send_ctl_set_packet_out_token (const struct lsquic_send_ctl *ctl,
                                        struct lsquic_packet_out *packet_out)
{
    unsigned char *token;

    token = malloc(ctl->sc_token_sz);
    if (!token)
    {
        LSQ_WARN("malloc failed: cannot set initial token");
        return -1;
    }

    memcpy(token, ctl->sc_token, ctl->sc_token_sz);
    packet_out->po_token = token;
    packet_out->po_token_len = ctl->sc_token_sz;
    packet_out->po_flags |= PO_NONCE;
    LSQ_DEBUG("set initial token on packet");
    return 0;
}


static lsquic_packet_out_t *
send_ctl_allocate_packet (struct lsquic_send_ctl *ctl, enum packno_bits bits,
                            unsigned need_at_least, enum packnum_space pns,
                            const struct network_path *path)
{
    lsquic_packet_out_t *packet_out;

    packet_out = lsquic_packet_out_new(&ctl->sc_enpub->enp_mm,
                    ctl->sc_conn_pub->packet_out_malo,
                    !(ctl->sc_flags & SC_TCID0), ctl->sc_conn_pub->lconn, bits,
                    ctl->sc_ver_neg->vn_tag, NULL, path);
    if (!packet_out)
        return NULL;

    if (need_at_least && lsquic_packet_out_avail(packet_out) < need_at_least)
    {   /* This should never happen, this is why this check is performed at
         * this level and not lower, before the packet is actually allocated.
         */
        LSQ_ERROR("wanted to allocate packet with at least %u bytes of "
            "payload, but only got %u bytes (mtu: %u bytes)", need_at_least,
            lsquic_packet_out_avail(packet_out), SC_PACK_SIZE(ctl));
        send_ctl_destroy_packet(ctl, packet_out);
        return NULL;
    }

    if (UNLIKELY(pns != PNS_APP))
    {
        if (pns == PNS_INIT)
        {
            packet_out->po_header_type = HETY_INITIAL;
            if (ctl->sc_token)
                (void) send_ctl_set_packet_out_token(ctl, packet_out);
        }
        else
            packet_out->po_header_type = HETY_HANDSHAKE;
    }

    lsquic_packet_out_set_pns(packet_out, pns);
    packet_out->po_lflags |= ctl->sc_ecn << POECN_SHIFT;
    packet_out->po_loss_chain = packet_out;
    return packet_out;
}


lsquic_packet_out_t *
lsquic_send_ctl_new_packet_out (lsquic_send_ctl_t *ctl, unsigned need_at_least,
                        enum packnum_space pns, const struct network_path *path)
{
    lsquic_packet_out_t *packet_out;
    enum packno_bits bits;

    bits = lsquic_send_ctl_packno_bits(ctl);
    packet_out = send_ctl_allocate_packet(ctl, bits, need_at_least, pns, path);
    if (!packet_out)
        return NULL;

    packet_out->po_packno = send_ctl_next_packno(ctl);
    LSQ_DEBUG("created packet %"PRIu64, packet_out->po_packno);
    EV_LOG_PACKET_CREATED(LSQUIC_LOG_CONN_ID, packet_out);
    return packet_out;
}


struct lsquic_packet_out *
lsquic_send_ctl_last_scheduled (struct lsquic_send_ctl *ctl,
                    enum packnum_space pns, const struct network_path *path,
                    int regen_match)
{
    struct lsquic_packet_out *packet_out;

    if (0 == regen_match)
    {
        TAILQ_FOREACH_REVERSE(packet_out, &ctl->sc_scheduled_packets,
                                                lsquic_packets_tailq, po_next)
            if (pns == lsquic_packet_out_pns(packet_out)
                                                && path == packet_out->po_path)
                return packet_out;
    }
    else
    {
        TAILQ_FOREACH_REVERSE(packet_out, &ctl->sc_scheduled_packets,
                                                lsquic_packets_tailq, po_next)
            if (pns == lsquic_packet_out_pns(packet_out)
                    && packet_out->po_regen_sz == packet_out->po_data_sz
                                                && path == packet_out->po_path)
                return packet_out;
    }

    return NULL;
}


/* Do not use for STREAM frames
 */
lsquic_packet_out_t *
lsquic_send_ctl_get_writeable_packet (lsquic_send_ctl_t *ctl,
                enum packnum_space pns, unsigned need_at_least,
                const struct network_path *path, int regen_match, int *is_err)
{
    lsquic_packet_out_t *packet_out;

    assert(need_at_least > 0);

    packet_out = lsquic_send_ctl_last_scheduled(ctl, pns, path, regen_match);
    if (packet_out
        && !(packet_out->po_flags & (PO_MINI|PO_STREAM_END|PO_RETX))
        && lsquic_packet_out_avail(packet_out) >= need_at_least)
    {
        return packet_out;
    }

    if (!lsquic_send_ctl_can_send(ctl))
    {
        if (is_err)
            *is_err = 0;
        return NULL;
    }

    packet_out = lsquic_send_ctl_new_packet_out(ctl, need_at_least, pns, path);
    if (packet_out)
    {
        lsquic_packet_out_set_pns(packet_out, pns);
        lsquic_send_ctl_scheduled_one(ctl, packet_out);
    }
    else if (is_err)
        *is_err = 1;
    return packet_out;
}


struct lsquic_packet_out *
lsquic_send_ctl_get_packet_for_crypto (struct lsquic_send_ctl *ctl,
                          unsigned need_at_least, enum packnum_space pns,
                          const struct network_path *path)
{
    struct lsquic_packet_out *packet_out;

    assert(lsquic_send_ctl_schedule_stream_packets_immediately(ctl));
    assert(need_at_least > 0);

    packet_out = lsquic_send_ctl_last_scheduled(ctl, pns, path, 0);
    if (packet_out
        && !(packet_out->po_flags & (PO_STREAM_END|PO_RETX))
        && lsquic_packet_out_avail(packet_out) >= need_at_least)
    {
        return packet_out;
    }

    if (!lsquic_send_ctl_can_send(ctl))
        return NULL;

    packet_out = lsquic_send_ctl_new_packet_out(ctl, need_at_least, pns, path);
    if (!packet_out)
        return NULL;

    lsquic_send_ctl_scheduled_one(ctl, packet_out);
    return packet_out;
}


static void
update_for_resending (lsquic_send_ctl_t *ctl, lsquic_packet_out_t *packet_out)
{

    lsquic_packno_t oldno, packno;

    /* When the packet is resent, it uses the same number of bytes to encode
     * the packet number as the original packet.  This follows the reference
     * implementation.
     */
    oldno = packet_out->po_packno;
    packno = send_ctl_next_packno(ctl);

    packet_out->po_flags &= ~PO_SENT_SZ;
    packet_out->po_frame_types &= ~GQUIC_FRAME_REGEN_MASK;
    assert(packet_out->po_frame_types);
    packet_out->po_packno = packno;
    lsquic_packet_out_set_ecn(packet_out, ctl->sc_ecn);

    if (ctl->sc_ver_neg->vn_tag)
    {
        assert(packet_out->po_flags & PO_VERSION);  /* It can only disappear */
        packet_out->po_ver_tag = *ctl->sc_ver_neg->vn_tag;
    }

    assert(packet_out->po_regen_sz < packet_out->po_data_sz);
    if (packet_out->po_regen_sz)
    {
        if (packet_out->po_flags & PO_SCHED)
            ctl->sc_bytes_scheduled -= packet_out->po_regen_sz;
        lsquic_packet_out_chop_regen(packet_out);
    }
    LSQ_DEBUG("Packet %"PRIu64" repackaged for resending as packet %"PRIu64,
                                                            oldno, packno);
    EV_LOG_CONN_EVENT(LSQUIC_LOG_CONN_ID, "packet %"PRIu64" repackaged for "
        "resending as packet %"PRIu64, oldno, packno);
}


unsigned
lsquic_send_ctl_reschedule_packets (lsquic_send_ctl_t *ctl)
{
    lsquic_packet_out_t *packet_out;
    unsigned n = 0;

    while ((packet_out = send_ctl_next_lost(ctl)))
    {
        assert(packet_out->po_regen_sz < packet_out->po_data_sz);
        ++n;
#if LSQUIC_CONN_STATS
        ++ctl->sc_conn_pub->conn_stats->out.retx_packets;
#endif
        update_for_resending(ctl, packet_out);
        lsquic_send_ctl_scheduled_one(ctl, packet_out);
    }

    if (n)
        LSQ_DEBUG("rescheduled %u packets", n);

    return n;
}


void
lsquic_send_ctl_set_tcid0 (lsquic_send_ctl_t *ctl, int tcid0)
{
    if (tcid0)
    {
        LSQ_INFO("set TCID flag");
        ctl->sc_flags |=  SC_TCID0;
    }
    else
    {
        LSQ_INFO("unset TCID flag");
        ctl->sc_flags &= ~SC_TCID0;
    }
}


/* The controller elides this STREAM frames of stream `stream_id' from
 * scheduled and buffered packets.  If a packet becomes empty as a result,
 * it is dropped.
 *
 * Packets on other queues do not need to be processed: unacked packets
 * have already been sent, and lost packets' reset stream frames will be
 * elided in due time.
 */
void
lsquic_send_ctl_elide_stream_frames (lsquic_send_ctl_t *ctl,
                                                lsquic_stream_id_t stream_id)
{
    struct lsquic_packet_out *packet_out, *next;
    unsigned n, adj;
    int dropped;

    dropped = 0;
#ifdef WIN32
    next = NULL;
#endif
    for (packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets); packet_out;
                                                            packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);

        if ((packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM))
                                    && 0 == (packet_out->po_flags & PO_MINI))
        {
            adj = lsquic_packet_out_elide_reset_stream_frames(packet_out,
                                                              stream_id);
            ctl->sc_bytes_scheduled -= adj;
            if (0 == packet_out->po_frame_types)
            {
                LSQ_DEBUG("cancel packet %"PRIu64" after eliding frames for "
                    "stream %"PRIu64, packet_out->po_packno, stream_id);
                send_ctl_sched_remove(ctl, packet_out);
                send_ctl_destroy_chain(ctl, packet_out, NULL);
                send_ctl_destroy_packet(ctl, packet_out);
                ++dropped;
            }
        }
    }

    if (dropped)
        lsquic_send_ctl_reset_packnos(ctl);

    for (n = 0; n < sizeof(ctl->sc_buffered_packets) /
                                sizeof(ctl->sc_buffered_packets[0]); ++n)
    {
        for (packet_out = TAILQ_FIRST(&ctl->sc_buffered_packets[n].bpq_packets);
                                                packet_out; packet_out = next)
        {
            next = TAILQ_NEXT(packet_out, po_next);
            if (packet_out->po_frame_types & (1 << QUIC_FRAME_STREAM))
            {
                lsquic_packet_out_elide_reset_stream_frames(packet_out, stream_id);
                if (0 == packet_out->po_frame_types)
                {
                    LSQ_DEBUG("cancel buffered packet in queue #%u after eliding "
                        "frames for stream %"PRIu64, n, stream_id);
                    TAILQ_REMOVE(&ctl->sc_buffered_packets[n].bpq_packets,
                                 packet_out, po_next);
                    --ctl->sc_buffered_packets[n].bpq_count;
                    send_ctl_destroy_packet(ctl, packet_out);
                    LSQ_DEBUG("Elide packet from buffered queue #%u; count: %u",
                              n, ctl->sc_buffered_packets[n].bpq_count);
                }
            }
        }
    }
}


/* Count how many packets will remain after the squeezing performed by
 * lsquic_send_ctl_squeeze_sched().  This is the number of delayed data
 * packets.
 */
#ifndef NDEBUG
#if __GNUC__
__attribute__((weak))
#endif
#endif
int
lsquic_send_ctl_have_delayed_packets (const lsquic_send_ctl_t *ctl)
{
    const struct lsquic_packet_out *packet_out;
    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        if (packet_out->po_regen_sz < packet_out->po_data_sz)
            return 1;
    return 0;
}


#ifndef NDEBUG
static void
send_ctl_log_packet_q (const lsquic_send_ctl_t *ctl, const char *prefix,
                                const struct lsquic_packets_tailq *tailq)
{
    const lsquic_packet_out_t *packet_out;
    unsigned n_packets;
    char *buf;
    size_t bufsz;
    int off;

    n_packets = 0;
    TAILQ_FOREACH(packet_out, tailq, po_next)
        ++n_packets;

    if (n_packets == 0)
    {
        LSQ_DEBUG("%s: [<empty set>]", prefix);
        return;
    }

    bufsz = n_packets * sizeof("18446744073709551615" /* UINT64_MAX */);
    buf = malloc(bufsz);
    if (!buf)
    {
        LSQ_ERROR("%s: malloc: %s", __func__, strerror(errno));
        return;
    }

    off = 0;
    TAILQ_FOREACH(packet_out, tailq, po_next)
    {
        if (off)
            buf[off++] = ' ';
        off += sprintf(buf + off, "%"PRIu64, packet_out->po_packno);
    }

    LSQ_DEBUG("%s: [%s]", prefix, buf);
    free(buf);
}

#define LOG_PACKET_Q(prefix, queue) do {                                    \
    if (LSQ_LOG_ENABLED(LSQ_LOG_DEBUG))                                     \
        send_ctl_log_packet_q(ctl, queue, prefix);                          \
} while (0)
#else
#define LOG_PACKET_Q(p, q)
#endif


int
lsquic_send_ctl_squeeze_sched (lsquic_send_ctl_t *ctl)
{
    struct lsquic_packet_out *packet_out, *next;
    int dropped;
#ifndef NDEBUG
    int pre_squeeze_logged = 0;
#endif

    dropped = 0;
    for (packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets); packet_out;
                                                            packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);
        if (packet_out->po_regen_sz < packet_out->po_data_sz)
        {
            if (packet_out->po_flags & PO_ENCRYPTED)
                send_ctl_return_enc_data(ctl, packet_out);
        }
        else
        {
#ifndef NDEBUG
            /* Log the whole list before we squeeze for the first time */
            if (!pre_squeeze_logged++)
                LOG_PACKET_Q(&ctl->sc_scheduled_packets,
                                        "unacked packets before squeezing");
#endif
            send_ctl_sched_remove(ctl, packet_out);
            LSQ_DEBUG("Dropping packet %"PRIu64" from scheduled queue",
                packet_out->po_packno);
            send_ctl_destroy_chain(ctl, packet_out, NULL);
            send_ctl_destroy_packet(ctl, packet_out);
            ++dropped;
        }
    }

    if (dropped)
        lsquic_send_ctl_reset_packnos(ctl);

#ifndef NDEBUG
    if (pre_squeeze_logged)
        LOG_PACKET_Q(&ctl->sc_scheduled_packets,
                                        "unacked packets after squeezing");
    else if (ctl->sc_n_scheduled > 0)
        LOG_PACKET_Q(&ctl->sc_scheduled_packets, "delayed packets");
#endif

    return ctl->sc_n_scheduled > 0;
}


void
lsquic_send_ctl_reset_packnos (lsquic_send_ctl_t *ctl)
{
    struct lsquic_packet_out *packet_out;

    ctl->sc_cur_packno = lsquic_senhist_largest(&ctl->sc_senhist);
    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        packet_out->po_flags |= PO_REPACKNO;
}


void
lsquic_send_ctl_ack_to_front (struct lsquic_send_ctl *ctl, unsigned n_acks)
{
    struct lsquic_packet_out *ack_packet;

    assert(n_acks > 0);
    assert(ctl->sc_n_scheduled > n_acks);   /* Otherwise, why is this called? */
    for ( ; n_acks > 0; --n_acks)
    {
        ack_packet = TAILQ_LAST(&ctl->sc_scheduled_packets, lsquic_packets_tailq);
        assert(ack_packet->po_frame_types & (1 << QUIC_FRAME_ACK));
        TAILQ_REMOVE(&ctl->sc_scheduled_packets, ack_packet, po_next);
        TAILQ_INSERT_HEAD(&ctl->sc_scheduled_packets, ack_packet, po_next);
    }
}


void
lsquic_send_ctl_drop_scheduled (lsquic_send_ctl_t *ctl)
{
    struct lsquic_packet_out *packet_out, *next;
    unsigned n;

    n = 0;
    for (packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets); packet_out;
                                                            packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);
        if (0 == (packet_out->po_flags & PO_HELLO))
        {
            send_ctl_sched_remove(ctl, packet_out);
            send_ctl_destroy_chain(ctl, packet_out, NULL);
            send_ctl_destroy_packet(ctl, packet_out);
            ++n;
        }
    }

    ctl->sc_senhist.sh_flags |= SH_GAP_OK;

    LSQ_DEBUG("dropped %u scheduled packet%s (%u left)", n, n != 1 ? "s" : "",
        ctl->sc_n_scheduled);
}


#ifdef NDEBUG
static
#elif __GNUC__
__attribute__((weak))
#endif
enum buf_packet_type
lsquic_send_ctl_determine_bpt (lsquic_send_ctl_t *ctl,
                                            const lsquic_stream_t *stream)
{
    const lsquic_stream_t *other_stream;
    struct lsquic_hash_elem *el;
    struct lsquic_hash *all_streams;

    all_streams = ctl->sc_conn_pub->all_streams;
    for (el = lsquic_hash_first(all_streams); el;
                                     el = lsquic_hash_next(all_streams))
    {
        other_stream = lsquic_hashelem_getdata(el);
        if (other_stream != stream
              && (!(other_stream->stream_flags & STREAM_U_WRITE_DONE))
                && !lsquic_stream_is_critical(other_stream)
                  && other_stream->sm_priority < stream->sm_priority)
            return BPT_OTHER_PRIO;
    }
    return BPT_HIGHEST_PRIO;
}


static enum buf_packet_type
send_ctl_lookup_bpt (lsquic_send_ctl_t *ctl,
                                        const struct lsquic_stream *stream)
{
    if (ctl->sc_cached_bpt.stream_id != stream->id)
    {
        ctl->sc_cached_bpt.stream_id = stream->id;
        ctl->sc_cached_bpt.packet_type =
                                lsquic_send_ctl_determine_bpt(ctl, stream);
    }
    return ctl->sc_cached_bpt.packet_type;
}


static unsigned
send_ctl_max_bpq_count (const lsquic_send_ctl_t *ctl,
                                        enum buf_packet_type packet_type)
{
    unsigned long cwnd;
    unsigned count;

    switch (packet_type)
    {
    case BPT_OTHER_PRIO:
        return MAX_BPQ_COUNT;
    case BPT_HIGHEST_PRIO:
    default: /* clang does not complain about absence of `default'... */
        count = ctl->sc_n_scheduled + ctl->sc_n_in_flight_retx;
        cwnd = ctl->sc_ci->cci_get_cwnd(CGP(ctl));
        if (count < cwnd / SC_PACK_SIZE(ctl))
        {
            count = cwnd / SC_PACK_SIZE(ctl) - count;
            if (count > MAX_BPQ_COUNT)
                return count;
        }
        return MAX_BPQ_COUNT;
    }
}


static void
send_ctl_move_ack (struct lsquic_send_ctl *ctl, struct lsquic_packet_out *dst,
                    struct lsquic_packet_out *src)
{
    assert(dst->po_data_sz == 0);

    if (lsquic_packet_out_avail(dst) >= src->po_regen_sz)
    {
        memcpy(dst->po_data, src->po_data, src->po_regen_sz);
        dst->po_data_sz = src->po_regen_sz;
        dst->po_regen_sz = src->po_regen_sz;
        dst->po_frame_types |= (GQUIC_FRAME_REGEN_MASK & src->po_frame_types);
        src->po_frame_types &= ~GQUIC_FRAME_REGEN_MASK;
        lsquic_packet_out_chop_regen(src);
    }
}


static lsquic_packet_out_t *
send_ctl_get_buffered_packet (lsquic_send_ctl_t *ctl,
            enum buf_packet_type packet_type, unsigned need_at_least,
            const struct network_path *path, const struct lsquic_stream *stream)
{
    struct buf_packet_q *const packet_q =
                                    &ctl->sc_buffered_packets[packet_type];
    struct lsquic_conn *const lconn = ctl->sc_conn_pub->lconn;
    lsquic_packet_out_t *packet_out;
    enum packno_bits bits;
    enum { AA_STEAL, AA_GENERATE, AA_NONE, } ack_action;

    packet_out = TAILQ_LAST(&packet_q->bpq_packets, lsquic_packets_tailq);
    if (packet_out
        && !(packet_out->po_flags & PO_STREAM_END)
        && lsquic_packet_out_avail(packet_out) >= need_at_least)
    {
        return packet_out;
    }

    if (packet_q->bpq_count >= send_ctl_max_bpq_count(ctl, packet_type))
        return NULL;

    if (packet_q->bpq_count == 0)
    {
        /* If ACK was written to the low-priority queue first, steal it */
        if (packet_q == &ctl->sc_buffered_packets[BPT_HIGHEST_PRIO]
            && !TAILQ_EMPTY(&ctl->sc_buffered_packets[BPT_OTHER_PRIO].bpq_packets)
            && (TAILQ_FIRST(&ctl->sc_buffered_packets[BPT_OTHER_PRIO].bpq_packets)
                                        ->po_frame_types & QUIC_FTBIT_ACK))
        {
            LSQ_DEBUG("steal ACK frame from low-priority buffered queue");
            ack_action = AA_STEAL;
            bits = ctl->sc_max_packno_bits;
        }
        /* If ACK can be generated, write it to the first buffered packet. */
        else if (lconn->cn_if->ci_can_write_ack(lconn))
        {
            LSQ_DEBUG("generate ACK frame for first buffered packet in "
                                                    "queue #%u", packet_type);
            ack_action = AA_GENERATE;
            /* Packet length is set to the largest possible size to guarantee
             * that buffered packet with the ACK will not need to be split.
             */
            bits = ctl->sc_max_packno_bits;
        }
        else
            goto no_ack_action;
    }
    else
    {
  no_ack_action:
        ack_action = AA_NONE;
        bits = lsquic_send_ctl_guess_packno_bits(ctl);
    }

    packet_out = send_ctl_allocate_packet(ctl, bits, need_at_least, PNS_APP,
                                                                        path);
    if (!packet_out)
        return NULL;

    switch (ack_action)
    {
    case AA_STEAL:
        send_ctl_move_ack(ctl, packet_out,
            TAILQ_FIRST(&ctl->sc_buffered_packets[BPT_OTHER_PRIO].bpq_packets));
        break;
    case AA_GENERATE:
        lconn->cn_if->ci_write_ack(lconn, packet_out);
        break;
    case AA_NONE:
        break;
    }

    TAILQ_INSERT_TAIL(&packet_q->bpq_packets, packet_out, po_next);
    ++packet_q->bpq_count;
    LSQ_DEBUG("Add new packet to buffered queue #%u; count: %u",
              packet_type, packet_q->bpq_count);
    return packet_out;
}


lsquic_packet_out_t *
lsquic_send_ctl_get_packet_for_stream (lsquic_send_ctl_t *ctl,
                unsigned need_at_least, const struct network_path *path,
                const struct lsquic_stream *stream)
{
    enum buf_packet_type packet_type;

    if (lsquic_send_ctl_schedule_stream_packets_immediately(ctl))
        return lsquic_send_ctl_get_writeable_packet(ctl, PNS_APP,
                                                need_at_least, path, 0, NULL);
    else
    {
        packet_type = send_ctl_lookup_bpt(ctl, stream);
        return send_ctl_get_buffered_packet(ctl, packet_type, need_at_least,
                                            path, stream);
    }
}


#ifdef NDEBUG
static
#elif __GNUC__
__attribute__((weak))
#endif
enum packno_bits
lsquic_send_ctl_calc_packno_bits (lsquic_send_ctl_t *ctl)
{
    lsquic_packno_t smallest_unacked;
    enum packno_bits bits;
    unsigned n_in_flight;
    unsigned long cwnd;
    const struct parse_funcs *pf;

    pf = ctl->sc_conn_pub->lconn->cn_pf;

    smallest_unacked = lsquic_send_ctl_smallest_unacked(ctl);
    cwnd = ctl->sc_ci->cci_get_cwnd(CGP(ctl));
    n_in_flight = cwnd / SC_PACK_SIZE(ctl);
    bits = pf->pf_calc_packno_bits(ctl->sc_cur_packno + 1, smallest_unacked,
                                                            n_in_flight);
    if (bits <= ctl->sc_max_packno_bits)
        return bits;
    else
        return ctl->sc_max_packno_bits;
}


enum packno_bits
lsquic_send_ctl_packno_bits (lsquic_send_ctl_t *ctl)
{

    if (lsquic_send_ctl_schedule_stream_packets_immediately(ctl))
        return lsquic_send_ctl_calc_packno_bits(ctl);
    else
        return lsquic_send_ctl_guess_packno_bits(ctl);
}


static int
split_buffered_packet (lsquic_send_ctl_t *ctl,
        enum buf_packet_type packet_type, lsquic_packet_out_t *packet_out,
        enum packno_bits bits, unsigned excess_bytes)
{
    struct buf_packet_q *const packet_q =
                                    &ctl->sc_buffered_packets[packet_type];
    lsquic_packet_out_t *new_packet_out;

    assert(TAILQ_FIRST(&packet_q->bpq_packets) == packet_out);

    new_packet_out = send_ctl_allocate_packet(ctl, bits, 0,
                        lsquic_packet_out_pns(packet_out), packet_out->po_path);
    if (!new_packet_out)
        return -1;

    if (0 == lsquic_packet_out_split_in_two(&ctl->sc_enpub->enp_mm, packet_out,
                  new_packet_out, ctl->sc_conn_pub->lconn->cn_pf, excess_bytes))
    {
        lsquic_packet_out_set_packno_bits(packet_out, bits);
        TAILQ_INSERT_AFTER(&packet_q->bpq_packets, packet_out, new_packet_out,
                           po_next);
        ++packet_q->bpq_count;
        LSQ_DEBUG("Add split packet to buffered queue #%u; count: %u",
                  packet_type, packet_q->bpq_count);
        return 0;
    }
    else
    {
        send_ctl_destroy_packet(ctl, new_packet_out);
        return -1;
    }
}


int
lsquic_send_ctl_schedule_buffered (lsquic_send_ctl_t *ctl,
                                            enum buf_packet_type packet_type)
{
    struct buf_packet_q *const packet_q =
                                    &ctl->sc_buffered_packets[packet_type];
    const struct parse_funcs *const pf = ctl->sc_conn_pub->lconn->cn_pf;
    lsquic_packet_out_t *packet_out;
    unsigned used, excess;

    assert(lsquic_send_ctl_schedule_stream_packets_immediately(ctl));
    const enum packno_bits bits = lsquic_send_ctl_calc_packno_bits(ctl);
    const unsigned need = pf->pf_packno_bits2len(bits);

    while ((packet_out = TAILQ_FIRST(&packet_q->bpq_packets)) &&
                                            lsquic_send_ctl_can_send(ctl))
    {
        if ((packet_out->po_frame_types & QUIC_FTBIT_ACK)
                            && packet_out->po_ack2ed < ctl->sc_largest_acked)
        {
            /* Chrome watches for a decrease in the value of the Largest
             * Observed field of the ACK frame and marks it as an error:
             * this is why we have to send out ACK in the order they were
             * generated.
             */
            LSQ_DEBUG("Remove out-of-order ACK from buffered packet");
            lsquic_packet_out_chop_regen(packet_out);
            if (packet_out->po_data_sz == 0)
            {
                LSQ_DEBUG("Dropping now-empty buffered packet");
                TAILQ_REMOVE(&packet_q->bpq_packets, packet_out, po_next);
                --packet_q->bpq_count;
                send_ctl_destroy_packet(ctl, packet_out);
                continue;
            }
        }
        if (bits != lsquic_packet_out_packno_bits(packet_out))
        {
            used = pf->pf_packno_bits2len(
                                lsquic_packet_out_packno_bits(packet_out));
            if (need > used
                && need - used > lsquic_packet_out_avail(packet_out))
            {
                excess = need - used - lsquic_packet_out_avail(packet_out);
                if (0 != split_buffered_packet(ctl, packet_type,
                                               packet_out, bits, excess))
                {
                    return -1;
                }
            }
        }
        TAILQ_REMOVE(&packet_q->bpq_packets, packet_out, po_next);
        --packet_q->bpq_count;
        packet_out->po_packno = send_ctl_next_packno(ctl);
        LSQ_DEBUG("Remove packet from buffered queue #%u; count: %u.  "
            "It becomes packet %"PRIu64, packet_type, packet_q->bpq_count,
            packet_out->po_packno);
        lsquic_send_ctl_scheduled_one(ctl, packet_out);
    }

    return 0;
}


int
lsquic_send_ctl_turn_on_fin (struct lsquic_send_ctl *ctl,
                             const struct lsquic_stream *stream)
{
    enum buf_packet_type packet_type;
    struct buf_packet_q *packet_q;
    lsquic_packet_out_t *packet_out;
    const struct parse_funcs *pf;

    pf = ctl->sc_conn_pub->lconn->cn_pf;
    packet_type = send_ctl_lookup_bpt(ctl, stream);
    packet_q = &ctl->sc_buffered_packets[packet_type];

    TAILQ_FOREACH_REVERSE(packet_out, &packet_q->bpq_packets,
                          lsquic_packets_tailq, po_next)
        if (0 == lsquic_packet_out_turn_on_fin(packet_out, pf, stream))
            return 0;

    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        if (0 == packet_out->po_sent
            && 0 == lsquic_packet_out_turn_on_fin(packet_out, pf, stream))
        {
            return 0;
        }

    return -1;
}


size_t
lsquic_send_ctl_mem_used (const struct lsquic_send_ctl *ctl)
{
    const lsquic_packet_out_t *packet_out;
    unsigned n;
    size_t size;
    const struct lsquic_packets_tailq queues[] = {
        ctl->sc_scheduled_packets,
        ctl->sc_unacked_packets[PNS_INIT],
        ctl->sc_unacked_packets[PNS_HSK],
        ctl->sc_unacked_packets[PNS_APP],
        ctl->sc_lost_packets,
        ctl->sc_buffered_packets[0].bpq_packets,
        ctl->sc_buffered_packets[1].bpq_packets,
    };

    size = sizeof(*ctl);

    for (n = 0; n < sizeof(queues) / sizeof(queues[0]); ++n)
        TAILQ_FOREACH(packet_out, &queues[n], po_next)
            size += lsquic_packet_out_mem_used(packet_out);

    return size;
}


void
lsquic_send_ctl_verneg_done (struct lsquic_send_ctl *ctl)
{
    ctl->sc_max_packno_bits = PACKNO_BITS_3;
    LSQ_DEBUG("version negotiation done (%s): max packno bits: %u",
        lsquic_ver2str[ ctl->sc_conn_pub->lconn->cn_version ],
        ctl->sc_max_packno_bits);
}


static void
strip_trailing_padding (struct lsquic_packet_out *packet_out)
{
    struct packet_out_srec_iter posi;
    const struct stream_rec *srec;
    unsigned off;

    off = 0;
    for (srec = posi_first(&posi, packet_out); srec; srec = posi_next(&posi))
        off = srec->sr_off + srec->sr_len;

    assert(off);

    packet_out->po_data_sz = off;
    packet_out->po_frame_types &= ~QUIC_FTBIT_PADDING;
}


int
lsquic_send_ctl_retry (struct lsquic_send_ctl *ctl,
                                const unsigned char *token, size_t token_sz)
{
    struct lsquic_packet_out *packet_out, *next, *new_packet_out;
    struct lsquic_conn *const lconn = ctl->sc_conn_pub->lconn;
    size_t sz;

    if (token_sz >= 1ull << (sizeof(packet_out->po_token_len) * 8))
    {
        LSQ_WARN("token size %zu is too long", token_sz);
        return -1;
    }

    ++ctl->sc_retry_count;
    if (ctl->sc_retry_count > 3)
    {
        LSQ_INFO("failing connection after %u retries", ctl->sc_retry_count);
        return -1;
    }

    send_ctl_expire(ctl, PNS_INIT, EXFI_ALL);

    if (0 != lsquic_send_ctl_set_token(ctl, token, token_sz))
        return -1;

    for (packet_out = TAILQ_FIRST(&ctl->sc_lost_packets); packet_out; packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);
        if (HETY_INITIAL != packet_out->po_header_type)
            continue;

        if (packet_out->po_nonce)
        {
            free(packet_out->po_nonce);
            packet_out->po_nonce = NULL;
            packet_out->po_flags &= ~PO_NONCE;
        }

        if (0 != send_ctl_set_packet_out_token(ctl, packet_out))
        {
            LSQ_INFO("cannot set out token on packet");
            return -1;
        }

        if (packet_out->po_frame_types & QUIC_FTBIT_PADDING)
            strip_trailing_padding(packet_out);

        sz = lconn->cn_pf->pf_packout_size(lconn, packet_out);
        if (sz > 1200)
        {
            const enum packno_bits bits = lsquic_send_ctl_calc_packno_bits(ctl);
            new_packet_out = send_ctl_allocate_packet(ctl, bits, 0, PNS_INIT,
                                                        packet_out->po_path);
            if (!new_packet_out)
                return -1;
            if (0 != send_ctl_set_packet_out_token(ctl, new_packet_out))
            {
                send_ctl_destroy_packet(ctl, new_packet_out);
                LSQ_INFO("cannot set out token on packet");
                return -1;
            }
            if (0 == lsquic_packet_out_split_in_two(&ctl->sc_enpub->enp_mm,
                            packet_out, new_packet_out,
                            ctl->sc_conn_pub->lconn->cn_pf, sz - 1200))
            {
                LSQ_DEBUG("split lost packet %"PRIu64" into two",
                                                        packet_out->po_packno);
                lsquic_packet_out_set_packno_bits(packet_out, bits);
                TAILQ_INSERT_AFTER(&ctl->sc_lost_packets, packet_out,
                                    new_packet_out, po_next);
                new_packet_out->po_flags |= PO_LOST;
                packet_out->po_flags &= ~PO_SENT_SZ;
            }
            else
            {
                LSQ_DEBUG("could not split lost packet into two");
                send_ctl_destroy_packet(ctl, new_packet_out);
                return -1;
            }
        }
    }

    return 0;
}


int
lsquic_send_ctl_set_token (struct lsquic_send_ctl *ctl,
                const unsigned char *token, size_t token_sz)
{
    unsigned char *copy;

    if (token_sz > 1 <<
                (sizeof(((struct lsquic_packet_out *)0)->po_token_len) * 8))
    {
        errno = EINVAL;
        return -1;
    }

    copy = malloc(token_sz);
    if (!copy)
        return -1;
    memcpy(copy, token, token_sz);
    free(ctl->sc_token);
    ctl->sc_token = copy;
    ctl->sc_token_sz = token_sz;
    LSQ_DEBUG("set token");
    return 0;
}


void
lsquic_send_ctl_empty_pns (struct lsquic_send_ctl *ctl, enum packnum_space pns)
{
    lsquic_packet_out_t *packet_out, *next;
    unsigned count, packet_sz;
    struct lsquic_packets_tailq *const *q;
    struct lsquic_packets_tailq *const queues[] = {
        &ctl->sc_lost_packets,
        &ctl->sc_buffered_packets[0].bpq_packets,
        &ctl->sc_buffered_packets[1].bpq_packets,
    };

    /* Don't bother with chain destruction, as all chains members are always
     * within the same packet number space
     */

    count = 0;
    for (packet_out = TAILQ_FIRST(&ctl->sc_scheduled_packets); packet_out;
                                                            packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);
        if (pns == lsquic_packet_out_pns(packet_out))
        {
            send_ctl_maybe_renumber_sched_to_right(ctl, packet_out);
            send_ctl_sched_remove(ctl, packet_out);
            send_ctl_destroy_packet(ctl, packet_out);
            ++count;
        }
    }

    for (packet_out = TAILQ_FIRST(&ctl->sc_unacked_packets[pns]); packet_out;
                                                            packet_out = next)
    {
        next = TAILQ_NEXT(packet_out, po_next);
        if (packet_out->po_flags & PO_LOSS_REC)
            TAILQ_REMOVE(&ctl->sc_unacked_packets[pns], packet_out, po_next);
        else
        {
            packet_sz = packet_out_sent_sz(packet_out);
            send_ctl_unacked_remove(ctl, packet_out, packet_sz);
            lsquic_packet_out_ack_streams(packet_out);
        }
        send_ctl_destroy_packet(ctl, packet_out);
        ++count;
    }

    for (q = queues; q < queues + sizeof(queues) / sizeof(queues[0]); ++q)
        for (packet_out = TAILQ_FIRST(*q); packet_out; packet_out = next)
            {
                next = TAILQ_NEXT(packet_out, po_next);
                if (pns == lsquic_packet_out_pns(packet_out))
                {
                    TAILQ_REMOVE(*q, packet_out, po_next);
                    send_ctl_destroy_packet(ctl, packet_out);
                    ++count;
                }
            }

    lsquic_alarmset_unset(ctl->sc_alset, AL_RETX_INIT + pns);

    LSQ_DEBUG("emptied %s, destroyed %u packet%.*s", lsquic_pns2str[pns],
        count, count != 1, "s");
}


void
lsquic_send_ctl_repath (struct lsquic_send_ctl *ctl, struct network_path *old,
                                                    struct network_path *new)
{
    struct lsquic_packet_out *packet_out;
    unsigned count;
    struct lsquic_packets_tailq *const *q;
    struct lsquic_packets_tailq *const queues[] = {
        &ctl->sc_scheduled_packets,
        &ctl->sc_unacked_packets[PNS_INIT],
        &ctl->sc_unacked_packets[PNS_HSK],
        &ctl->sc_unacked_packets[PNS_APP],
        &ctl->sc_lost_packets,
        &ctl->sc_buffered_packets[0].bpq_packets,
        &ctl->sc_buffered_packets[1].bpq_packets,
    };

    assert(ctl->sc_flags & SC_IETF);

    count = 0;
    for (q = queues; q < queues + sizeof(queues) / sizeof(queues[0]); ++q)
        TAILQ_FOREACH(packet_out, *q, po_next)
            if (packet_out->po_path == old)
            {
                ++count;
                packet_out->po_path = new;
                if (packet_out->po_flags & PO_ENCRYPTED)
                    send_ctl_return_enc_data(ctl, packet_out);
            }

    LSQ_DEBUG("repathed %u packet%.*s", count, count != 1, "s");
}


void
lsquic_send_ctl_return_enc_data (struct lsquic_send_ctl *ctl)
{
    struct lsquic_packet_out *packet_out;

    assert(!(ctl->sc_flags & SC_IETF));

    TAILQ_FOREACH(packet_out, &ctl->sc_scheduled_packets, po_next)
        if (packet_out->po_flags & PO_ENCRYPTED)
            send_ctl_return_enc_data(ctl, packet_out);
}
