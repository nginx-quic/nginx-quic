
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_quic_transport.h>


#define NGX_QUIC_LONG_DCID_LEN_OFFSET  5
#define NGX_QUIC_LONG_DCID_OFFSET      6
#define NGX_QUIC_SHORT_DCID_OFFSET     1


#if (NGX_HAVE_NONALIGNED)

#define ngx_quic_parse_uint16(p)  ntohs(*(uint16_t *) (p))
#define ngx_quic_parse_uint32(p)  ntohl(*(uint32_t *) (p))

#define ngx_quic_write_uint16  ngx_quic_write_uint16_aligned
#define ngx_quic_write_uint32  ngx_quic_write_uint32_aligned

#else

#define ngx_quic_parse_uint16(p)  ((p)[0] << 8 | (p)[1])
#define ngx_quic_parse_uint32(p)                                              \
    ((uint32_t) (p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3])

#define ngx_quic_write_uint16(p, s)                                           \
    ((p)[0] = (u_char) ((s) >> 8),                                            \
     (p)[1] = (u_char)  (s),                                                  \
     (p) + sizeof(uint16_t))

#define ngx_quic_write_uint32(p, s)                                           \
    ((p)[0] = (u_char) ((s) >> 24),                                           \
     (p)[1] = (u_char) ((s) >> 16),                                           \
     (p)[2] = (u_char) ((s) >> 8),                                            \
     (p)[3] = (u_char)  (s),                                                  \
     (p) + sizeof(uint32_t))

#endif

#define ngx_quic_write_uint64(p, s)                                           \
    ((p)[0] = (u_char) ((s) >> 56),                                           \
     (p)[1] = (u_char) ((s) >> 48),                                           \
     (p)[2] = (u_char) ((s) >> 40),                                           \
     (p)[3] = (u_char) ((s) >> 32),                                           \
     (p)[4] = (u_char) ((s) >> 24),                                           \
     (p)[5] = (u_char) ((s) >> 16),                                           \
     (p)[6] = (u_char) ((s) >> 8),                                            \
     (p)[7] = (u_char)  (s),                                                  \
     (p) + sizeof(uint64_t))

#define ngx_quic_write_uint24(p, s)                                           \
    ((p)[0] = (u_char) ((s) >> 16),                                           \
     (p)[1] = (u_char) ((s) >> 8),                                            \
     (p)[2] = (u_char)  (s),                                                  \
     (p) + 3)

#define ngx_quic_write_uint16_aligned(p, s)                                   \
    (*(uint16_t *) (p) = htons((uint16_t) (s)), (p) + sizeof(uint16_t))

#define ngx_quic_write_uint32_aligned(p, s)                                   \
    (*(uint32_t *) (p) = htonl((uint32_t) (s)), (p) + sizeof(uint32_t))

#define NGX_QUIC_VERSION(c)       (0xff000000 + (c))


static u_char *ngx_quic_parse_int(u_char *pos, u_char *end, uint64_t *out);
static ngx_uint_t ngx_quic_varint_len(uint64_t value);
static void ngx_quic_build_int(u_char **pos, uint64_t value);

static u_char *ngx_quic_read_uint8(u_char *pos, u_char *end, uint8_t *value);
static u_char *ngx_quic_read_uint32(u_char *pos, u_char *end, uint32_t *value);
static u_char *ngx_quic_read_bytes(u_char *pos, u_char *end, size_t len,
    u_char **out);
static u_char *ngx_quic_copy_bytes(u_char *pos, u_char *end, size_t len,
    u_char *dst);

static ngx_int_t ngx_quic_parse_short_header(ngx_quic_header_t *pkt,
    size_t dcid_len);
static ngx_int_t ngx_quic_parse_long_header(ngx_quic_header_t *pkt);
static ngx_int_t ngx_quic_supported_version(uint32_t version);
static ngx_int_t ngx_quic_parse_long_header_v1(ngx_quic_header_t *pkt);

static size_t ngx_quic_create_long_header(ngx_quic_header_t *pkt, u_char *out,
    size_t pkt_len, u_char **pnp);
static size_t ngx_quic_create_short_header(ngx_quic_header_t *pkt, u_char *out,
    size_t pkt_len, u_char **pnp);

static ngx_int_t ngx_quic_frame_allowed(ngx_quic_header_t *pkt,
    ngx_uint_t frame_type);
static size_t ngx_quic_create_ack(u_char *p, ngx_quic_ack_frame_t *ack,
    ngx_chain_t *ranges);
static size_t ngx_quic_create_stop_sending(u_char *p,
    ngx_quic_stop_sending_frame_t *ss);
static size_t ngx_quic_create_crypto(u_char *p,
    ngx_quic_crypto_frame_t *crypto, ngx_chain_t *data);
static size_t ngx_quic_create_hs_done(u_char *p);
static size_t ngx_quic_create_new_token(u_char *p,
    ngx_quic_new_token_frame_t *token);
static size_t ngx_quic_create_stream(u_char *p, ngx_quic_stream_frame_t *sf,
    ngx_chain_t *data);
static size_t ngx_quic_create_max_streams(u_char *p,
    ngx_quic_max_streams_frame_t *ms);
static size_t ngx_quic_create_max_stream_data(u_char *p,
    ngx_quic_max_stream_data_frame_t *ms);
static size_t ngx_quic_create_max_data(u_char *p,
    ngx_quic_max_data_frame_t *md);
static size_t ngx_quic_create_path_response(u_char *p,
    ngx_quic_path_challenge_frame_t *pc);
static size_t ngx_quic_create_new_connection_id(u_char *p,
    ngx_quic_new_conn_id_frame_t *rcid);
static size_t ngx_quic_create_retire_connection_id(u_char *p,
    ngx_quic_retire_cid_frame_t *rcid);
static size_t ngx_quic_create_close(u_char *p, ngx_quic_close_frame_t *cl);

static ngx_int_t ngx_quic_parse_transport_param(u_char *p, u_char *end,
    uint16_t id, ngx_quic_tp_t *dst);


uint32_t  ngx_quic_versions[] = {
#if (NGX_QUIC_DRAFT_VERSION >= 29)
    /* pretend we support all versions in range draft-29..v1 */
    NGX_QUIC_VERSION(29),
    NGX_QUIC_VERSION(30),
    NGX_QUIC_VERSION(31),
    NGX_QUIC_VERSION(32),
    /* QUICv1 */
    0x00000001
#else
    NGX_QUIC_VERSION(NGX_QUIC_DRAFT_VERSION)
#endif
};

#define NGX_QUIC_NVERSIONS \
    (sizeof(ngx_quic_versions) / sizeof(ngx_quic_versions[0]))


/* literal errors indexed by corresponding value */
static char *ngx_quic_errors[] = {
    "NO_ERROR",
    "INTERNAL_ERROR",
    "CONNECTION_REFUSED",
    "FLOW_CONTROL_ERROR",
    "STREAM_LIMIT_ERROR",
    "STREAM_STATE_ERROR",
    "FINAL_SIZE_ERROR",
    "FRAME_ENCODING_ERROR",
    "TRANSPORT_PARAMETER_ERROR",
    "CONNECTION_ID_LIMIT_ERROR",
    "PROTOCOL_VIOLATION",
    "INVALID_TOKEN",
    "APPLICATION_ERROR",
    "CRYPTO_BUFFER_EXCEEDED",
    "KEY_UPDATE_ERROR",
};


static ngx_inline u_char *
ngx_quic_parse_int(u_char *pos, u_char *end, uint64_t *out)
{
    u_char      *p;
    uint64_t     value;
    ngx_uint_t   len;

    if (pos >= end) {
        return NULL;
    }

    p = pos;
    len = 1 << (*p >> 6);

    value = *p++ & 0x3f;

    if ((size_t)(end - p) < (len - 1)) {
        return NULL;
    }

    while (--len) {
        value = (value << 8) + *p++;
    }

    *out = value;

    return p;
}


static ngx_inline u_char *
ngx_quic_read_uint8(u_char *pos, u_char *end, uint8_t *value)
{
    if ((size_t)(end - pos) < 1) {
        return NULL;
    }

    *value = *pos;

    return pos + 1;
}


static ngx_inline u_char *
ngx_quic_read_uint32(u_char *pos, u_char *end, uint32_t *value)
{
    if ((size_t)(end - pos) < sizeof(uint32_t)) {
        return NULL;
    }

    *value = ngx_quic_parse_uint32(pos);

    return pos + sizeof(uint32_t);
}


static ngx_inline u_char *
ngx_quic_read_bytes(u_char *pos, u_char *end, size_t len, u_char **out)
{
    if ((size_t)(end - pos) < len) {
        return NULL;
    }

    *out = pos;

    return pos + len;
}


static u_char *
ngx_quic_copy_bytes(u_char *pos, u_char *end, size_t len, u_char *dst)
{
    if ((size_t)(end - pos) < len) {
        return NULL;
    }

    ngx_memcpy(dst, pos, len);

    return pos + len;
}


static ngx_uint_t
ngx_quic_varint_len(uint64_t value)
{
    ngx_uint_t  bits;

    bits = 0;
    while (value >> ((8 << bits) - 2)) {
        bits++;
    }

    return 1 << bits;
}


static void
ngx_quic_build_int(u_char **pos, uint64_t value)
{
    u_char      *p;
    ngx_uint_t   bits, len;

    p = *pos;
    bits = 0;

    while (value >> ((8 << bits) - 2)) {
        bits++;
    }

    len = (1 << bits);

    while (len--) {
        *p++ = value >> (len * 8);
    }

    **pos |= bits << 6;
    *pos = p;
}


u_char *
ngx_quic_error_text(uint64_t error_code)
{
    if (error_code >= NGX_QUIC_ERR_CRYPTO_ERROR) {
        return (u_char *) "handshake error";
    }

    if (error_code >= NGX_QUIC_ERR_LAST) {
        return (u_char *) "unknown error";
    }

    return (u_char *) ngx_quic_errors[error_code];
}


ngx_int_t
ngx_quic_parse_packet(ngx_quic_header_t *pkt)
{
    if (!ngx_quic_long_pkt(pkt->flags)) {
        pkt->level = ssl_encryption_application;

        if (ngx_quic_parse_short_header(pkt, NGX_QUIC_SERVER_CID_LEN) != NGX_OK)
        {
            return NGX_DECLINED;
        }

        return NGX_OK;
    }

    if (ngx_quic_parse_long_header(pkt) != NGX_OK) {
        return NGX_DECLINED;
    }

    if (!ngx_quic_supported_version(pkt->version)) {
        return NGX_ABORT;
    }

    if (ngx_quic_parse_long_header_v1(pkt) != NGX_OK) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_quic_parse_short_header(ngx_quic_header_t *pkt, size_t dcid_len)
{
    u_char  *p, *end;

    p = pkt->raw->pos;
    end = pkt->data + pkt->len;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, pkt->log, 0,
                   "quic packet rx short flags:%xd", pkt->flags);

    if (!(pkt->flags & NGX_QUIC_PKT_FIXED_BIT)) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic fixed bit is not set");
        return NGX_ERROR;
    }

    pkt->dcid.len = dcid_len;

    p = ngx_quic_read_bytes(p, end, dcid_len, &pkt->dcid.data);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read dcid");
        return NGX_ERROR;
    }

    pkt->raw->pos = p;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_parse_long_header(ngx_quic_header_t *pkt)
{
    u_char   *p, *end;
    uint8_t   idlen;

    p = pkt->raw->pos;
    end = pkt->data + pkt->len;

    p = ngx_quic_read_uint32(p, end, &pkt->version);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read version");
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, pkt->log, 0,
                   "quic packet rx long flags:%xd version:%xD",
                   pkt->flags, pkt->version);

    if (!(pkt->flags & NGX_QUIC_PKT_FIXED_BIT)) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic fixed bit is not set");
        return NGX_ERROR;
    }

    p = ngx_quic_read_uint8(p, end, &idlen);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read dcid len");
        return NGX_ERROR;
    }

    if (idlen > NGX_QUIC_CID_LEN_MAX) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet dcid is too long");
        return NGX_ERROR;
    }

    pkt->dcid.len = idlen;

    p = ngx_quic_read_bytes(p, end, idlen, &pkt->dcid.data);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read dcid");
        return NGX_ERROR;
    }

    p = ngx_quic_read_uint8(p, end, &idlen);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read scid len");
        return NGX_ERROR;
    }

    if (idlen > NGX_QUIC_CID_LEN_MAX) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet scid is too long");
        return NGX_ERROR;
    }

    pkt->scid.len = idlen;

    p = ngx_quic_read_bytes(p, end, idlen, &pkt->scid.data);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic packet is too small to read scid");
        return NGX_ERROR;
    }

    pkt->raw->pos = p;

    return NGX_OK;
}


static ngx_int_t
ngx_quic_supported_version(uint32_t version)
{
    ngx_uint_t  i;

    for (i = 0; i < NGX_QUIC_NVERSIONS; i++) {
        if (ngx_quic_versions[i] == version) {
            return 1;
        }
    }

    return 0;
}


static ngx_int_t
ngx_quic_parse_long_header_v1(ngx_quic_header_t *pkt)
{
    u_char    *p, *end;
    uint64_t   varint;

    p = pkt->raw->pos;
    end = pkt->raw->last;

    pkt->log->action = "parsing quic long header";

    if (ngx_quic_pkt_in(pkt->flags)) {

        if (pkt->len < NGX_QUIC_MIN_INITIAL_SIZE) {
            ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                          "quic UDP datagram is too small for initial packet");
            return NGX_DECLINED;
        }

        p = ngx_quic_parse_int(p, end, &varint);
        if (p == NULL) {
            ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                          "quic failed to parse token length");
            return NGX_ERROR;
        }

        pkt->token.len = varint;

        p = ngx_quic_read_bytes(p, end, pkt->token.len, &pkt->token.data);
        if (p == NULL) {
            ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                          "quic packet too small to read token data");
            return NGX_ERROR;
        }

        pkt->level = ssl_encryption_initial;

    } else if (ngx_quic_pkt_zrtt(pkt->flags)) {
        pkt->level = ssl_encryption_early_data;

    } else if (ngx_quic_pkt_hs(pkt->flags)) {
        pkt->level = ssl_encryption_handshake;

    } else {
         ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                       "quic bad packet type");
         return NGX_DECLINED;
    }

    p = ngx_quic_parse_int(p, end, &varint);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic bad packet length");
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, pkt->log, 0,
                   "quic packet rx %s len:%uL",
                   ngx_quic_level_name(pkt->level), varint);

    if (varint > (uint64_t) ((pkt->data + pkt->len) - p)) {
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0, "quic truncated %s packet",
                      ngx_quic_level_name(pkt->level));
        return NGX_ERROR;
    }

    pkt->raw->pos = p;
    pkt->len = p + varint - pkt->data;

    return NGX_OK;
}


ngx_int_t
ngx_quic_get_packet_dcid(ngx_log_t *log, u_char *data, size_t n,
    ngx_str_t *dcid)
{
    size_t  len, offset;

    if (n == 0) {
        goto failed;
    }

    if (ngx_quic_long_pkt(*data)) {
        if (n < NGX_QUIC_LONG_DCID_LEN_OFFSET + 1) {
            goto failed;
        }

        len = data[NGX_QUIC_LONG_DCID_LEN_OFFSET];
        offset = NGX_QUIC_LONG_DCID_OFFSET;

    } else {
        len = NGX_QUIC_SERVER_CID_LEN;
        offset = NGX_QUIC_SHORT_DCID_OFFSET;
    }

    if (n < len + offset) {
        goto failed;
    }

    dcid->len = len;
    dcid->data = &data[offset];

    return NGX_OK;

failed:

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, log, 0, "quic malformed packet");

    return NGX_ERROR;
}


size_t
ngx_quic_create_version_negotiation(ngx_quic_header_t *pkt, u_char *out)
{
    u_char      *p, *start;
    ngx_uint_t   i;

    p = start = out;

    *p++ = pkt->flags;

    /*
     * The Version field of a Version Negotiation packet
     * MUST be set to 0x00000000
     */
    p = ngx_quic_write_uint32(p, 0);

    *p++ = pkt->dcid.len;
    p = ngx_cpymem(p, pkt->dcid.data, pkt->dcid.len);

    *p++ = pkt->scid.len;
    p = ngx_cpymem(p, pkt->scid.data, pkt->scid.len);

    for (i = 0; i < NGX_QUIC_NVERSIONS; i++) {
        p = ngx_quic_write_uint32(p, ngx_quic_versions[i]);
    }

    return p - start;
}


size_t
ngx_quic_create_header(ngx_quic_header_t *pkt, u_char *out, size_t pkt_len,
    u_char **pnp)
{
    return ngx_quic_short_pkt(pkt->flags)
           ? ngx_quic_create_short_header(pkt, out, pkt_len, pnp)
           : ngx_quic_create_long_header(pkt, out, pkt_len, pnp);
}


static size_t
ngx_quic_create_long_header(ngx_quic_header_t *pkt, u_char *out,
    size_t pkt_len, u_char **pnp)
{
    u_char  *p, *start;

    if (out == NULL) {
        return 5 + 2 + pkt->dcid.len + pkt->scid.len
               + ngx_quic_varint_len(pkt_len + pkt->num_len) + pkt->num_len
               + (pkt->level == ssl_encryption_initial ? 1 : 0);
    }

    p = start = out;

    *p++ = pkt->flags;

    p = ngx_quic_write_uint32(p, pkt->version);

    *p++ = pkt->dcid.len;
    p = ngx_cpymem(p, pkt->dcid.data, pkt->dcid.len);

    *p++ = pkt->scid.len;
    p = ngx_cpymem(p, pkt->scid.data, pkt->scid.len);

    if (pkt->level == ssl_encryption_initial) {
        ngx_quic_build_int(&p, 0);
    }

    ngx_quic_build_int(&p, pkt_len + pkt->num_len);

    *pnp = p;

    switch (pkt->num_len) {
    case 1:
        *p++ = pkt->trunc;
        break;
    case 2:
        p = ngx_quic_write_uint16(p, pkt->trunc);
        break;
    case 3:
        p = ngx_quic_write_uint24(p, pkt->trunc);
        break;
    case 4:
        p = ngx_quic_write_uint32(p, pkt->trunc);
        break;
    }

    return p - start;
}


static size_t
ngx_quic_create_short_header(ngx_quic_header_t *pkt, u_char *out,
    size_t pkt_len, u_char **pnp)
{
    u_char  *p, *start;

    if (out == NULL) {
        return 1 + pkt->dcid.len + pkt->num_len;
    }

    p = start = out;

    *p++ = pkt->flags;

    p = ngx_cpymem(p, pkt->dcid.data, pkt->dcid.len);

    *pnp = p;

    switch (pkt->num_len) {
    case 1:
        *p++ = pkt->trunc;
        break;
    case 2:
        p = ngx_quic_write_uint16(p, pkt->trunc);
        break;
    case 3:
        p = ngx_quic_write_uint24(p, pkt->trunc);
        break;
    case 4:
        p = ngx_quic_write_uint32(p, pkt->trunc);
        break;
    }

    return p - start;
}


size_t
ngx_quic_create_retry_itag(ngx_quic_header_t *pkt, u_char *out,
    u_char **start)
{
    u_char  *p;

    p = out;

    *p++ = pkt->odcid.len;
    p = ngx_cpymem(p, pkt->odcid.data, pkt->odcid.len);

    *start = p;

    *p++ = 0xff;

    p = ngx_quic_write_uint32(p, pkt->version);

    *p++ = pkt->dcid.len;
    p = ngx_cpymem(p, pkt->dcid.data, pkt->dcid.len);

    *p++ = pkt->scid.len;
    p = ngx_cpymem(p, pkt->scid.data, pkt->scid.len);

    p = ngx_cpymem(p, pkt->token.data, pkt->token.len);

    return p - out;
}


#define ngx_quic_stream_bit_off(val)  (((val) & 0x04) ? 1 : 0)
#define ngx_quic_stream_bit_len(val)  (((val) & 0x02) ? 1 : 0)
#define ngx_quic_stream_bit_fin(val)  (((val) & 0x01) ? 1 : 0)

ssize_t
ngx_quic_parse_frame(ngx_quic_header_t *pkt, u_char *start, u_char *end,
    ngx_quic_frame_t *f)
{
    u_char      *p;
    uint64_t     varint;
    ngx_buf_t   *b;
    ngx_uint_t   i;

    b = f->data->buf;

    p = start;

    p = ngx_quic_parse_int(p, end, &varint);
    if (p == NULL) {
        pkt->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                     "quic failed to obtain quic frame type");
        return NGX_ERROR;
    }

    f->type = varint;

    if (ngx_quic_frame_allowed(pkt, f->type) != NGX_OK) {
        pkt->error = NGX_QUIC_ERR_PROTOCOL_VIOLATION;
        return NGX_ERROR;
    }

    switch (f->type) {

    case NGX_QUIC_FT_CRYPTO:

        p = ngx_quic_parse_int(p, end, &f->u.crypto.offset);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_parse_int(p, end, &f->u.crypto.length);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_read_bytes(p, end, f->u.crypto.length, &b->pos);
        if (p == NULL) {
            goto error;
        }

        b->last = p;

        break;

    case NGX_QUIC_FT_PADDING:

        while (p < end && *p == NGX_QUIC_FT_PADDING) {
            p++;
        }

        break;

    case NGX_QUIC_FT_ACK:
    case NGX_QUIC_FT_ACK_ECN:

        if (!((p = ngx_quic_parse_int(p, end, &f->u.ack.largest))
              && (p = ngx_quic_parse_int(p, end, &f->u.ack.delay))
              && (p = ngx_quic_parse_int(p, end, &f->u.ack.range_count))
              && (p = ngx_quic_parse_int(p, end, &f->u.ack.first_range))))
        {
            goto error;
        }

        b->pos = p;

        /* process all ranges to get bounds, values are ignored */
        for (i = 0; i < f->u.ack.range_count; i++) {

            p = ngx_quic_parse_int(p, end, &varint);
            if (p) {
                p = ngx_quic_parse_int(p, end, &varint);
            }

            if (p == NULL) {
                goto error;
            }
        }

        b->last = p;

        f->u.ack.ranges_length = b->last - b->pos;

        if (f->type == NGX_QUIC_FT_ACK_ECN) {

            if (!((p = ngx_quic_parse_int(p, end, &f->u.ack.ect0))
                  && (p = ngx_quic_parse_int(p, end, &f->u.ack.ect1))
                  && (p = ngx_quic_parse_int(p, end, &f->u.ack.ce))))
            {
                goto error;
            }

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, pkt->log, 0,
                           "quic ACK ECN counters ect0:%uL ect1:%uL ce:%uL",
                           f->u.ack.ect0, f->u.ack.ect1, f->u.ack.ce);
        }

        break;

    case NGX_QUIC_FT_PING:
        break;

    case NGX_QUIC_FT_NEW_CONNECTION_ID:

        p = ngx_quic_parse_int(p, end, &f->u.ncid.seqnum);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_parse_int(p, end, &f->u.ncid.retire);
        if (p == NULL) {
            goto error;
        }

        if (f->u.ncid.retire > f->u.ncid.seqnum) {
            goto error;
        }

        p = ngx_quic_read_uint8(p, end, &f->u.ncid.len);
        if (p == NULL) {
            goto error;
        }

        if (f->u.ncid.len < 1 || f->u.ncid.len > NGX_QUIC_CID_LEN_MAX) {
            goto error;
        }

        p = ngx_quic_copy_bytes(p, end, f->u.ncid.len, f->u.ncid.cid);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_copy_bytes(p, end, NGX_QUIC_SR_TOKEN_LEN, f->u.ncid.srt);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_RETIRE_CONNECTION_ID:

        p = ngx_quic_parse_int(p, end, &f->u.retire_cid.sequence_number);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_CONNECTION_CLOSE:
    case NGX_QUIC_FT_CONNECTION_CLOSE_APP:

        p = ngx_quic_parse_int(p, end, &f->u.close.error_code);
        if (p == NULL) {
            goto error;
        }

        if (f->type == NGX_QUIC_FT_CONNECTION_CLOSE) {
            p = ngx_quic_parse_int(p, end, &f->u.close.frame_type);
            if (p == NULL) {
                goto error;
            }
        }

        p = ngx_quic_parse_int(p, end, &varint);
        if (p == NULL) {
            goto error;
        }

        f->u.close.reason.len = varint;

        p = ngx_quic_read_bytes(p, end, f->u.close.reason.len,
                                &f->u.close.reason.data);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_STREAM0:
    case NGX_QUIC_FT_STREAM1:
    case NGX_QUIC_FT_STREAM2:
    case NGX_QUIC_FT_STREAM3:
    case NGX_QUIC_FT_STREAM4:
    case NGX_QUIC_FT_STREAM5:
    case NGX_QUIC_FT_STREAM6:
    case NGX_QUIC_FT_STREAM7:

        f->u.stream.type = f->type;

        f->u.stream.off = ngx_quic_stream_bit_off(f->type);
        f->u.stream.len = ngx_quic_stream_bit_len(f->type);
        f->u.stream.fin = ngx_quic_stream_bit_fin(f->type);

        p = ngx_quic_parse_int(p, end, &f->u.stream.stream_id);
        if (p == NULL) {
            goto error;
        }

        if (f->type & 0x04) {
            p = ngx_quic_parse_int(p, end, &f->u.stream.offset);
            if (p == NULL) {
                goto error;
            }

        } else {
            f->u.stream.offset = 0;
        }

        if (f->type & 0x02) {
            p = ngx_quic_parse_int(p, end, &f->u.stream.length);
            if (p == NULL) {
                goto error;
            }

        } else {
            f->u.stream.length = end - p; /* up to packet end */
        }

        p = ngx_quic_read_bytes(p, end, f->u.stream.length, &b->pos);
        if (p == NULL) {
            goto error;
        }

        b->last = p;
        break;

    case NGX_QUIC_FT_MAX_DATA:

        p = ngx_quic_parse_int(p, end, &f->u.max_data.max_data);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_RESET_STREAM:

        if (!((p = ngx_quic_parse_int(p, end, &f->u.reset_stream.id))
              && (p = ngx_quic_parse_int(p, end, &f->u.reset_stream.error_code))
              && (p = ngx_quic_parse_int(p, end,
                                         &f->u.reset_stream.final_size))))
        {
            goto error;
        }

        break;

    case NGX_QUIC_FT_STOP_SENDING:

        p = ngx_quic_parse_int(p, end, &f->u.stop_sending.id);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_parse_int(p, end, &f->u.stop_sending.error_code);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_STREAMS_BLOCKED:
    case NGX_QUIC_FT_STREAMS_BLOCKED2:

        p = ngx_quic_parse_int(p, end, &f->u.streams_blocked.limit);
        if (p == NULL) {
            goto error;
        }

        f->u.streams_blocked.bidi =
                              (f->type == NGX_QUIC_FT_STREAMS_BLOCKED) ? 1 : 0;
        break;

    case NGX_QUIC_FT_MAX_STREAMS:
    case NGX_QUIC_FT_MAX_STREAMS2:

        p = ngx_quic_parse_int(p, end, &f->u.max_streams.limit);
        if (p == NULL) {
            goto error;
        }

        f->u.max_streams.bidi = (f->type == NGX_QUIC_FT_MAX_STREAMS) ? 1 : 0;

        break;

    case NGX_QUIC_FT_MAX_STREAM_DATA:

        p = ngx_quic_parse_int(p, end, &f->u.max_stream_data.id);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_parse_int(p, end,  &f->u.max_stream_data.limit);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_DATA_BLOCKED:

        p = ngx_quic_parse_int(p, end, &f->u.data_blocked.limit);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_STREAM_DATA_BLOCKED:

        p = ngx_quic_parse_int(p, end, &f->u.stream_data_blocked.id);
        if (p == NULL) {
            goto error;
        }

        p = ngx_quic_parse_int(p, end, &f->u.stream_data_blocked.limit);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_PATH_CHALLENGE:

        p = ngx_quic_copy_bytes(p, end, 8, f->u.path_challenge.data);
        if (p == NULL) {
            goto error;
        }

        break;

    case NGX_QUIC_FT_PATH_RESPONSE:

        p = ngx_quic_copy_bytes(p, end, 8, f->u.path_response.data);
        if (p == NULL) {
            goto error;
        }

        break;

    default:
        ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                      "quic unknown frame type 0x%xi", f->type);
        return NGX_ERROR;
    }

    f->level = pkt->level;

    return p - start;

error:

    pkt->error = NGX_QUIC_ERR_FRAME_ENCODING_ERROR;

    ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                  "quic failed to parse frame type:0x%xi", f->type);

    return NGX_ERROR;
}


static ngx_int_t
ngx_quic_frame_allowed(ngx_quic_header_t *pkt, ngx_uint_t frame_type)
{
    uint8_t  ptype;

    /* frame permissions per packet: 4 bits: IH01: 12.4, Table 3 */
    static uint8_t ngx_quic_frame_masks[] = {
         /* PADDING  */              0xF,
         /* PING */                  0xF,
         /* ACK */                   0xD,
         /* ACK_ECN */               0xD,
         /* RESET_STREAM */          0x3,
         /* STOP_SENDING */          0x3,
         /* CRYPTO */                0xD,
         /* NEW_TOKEN */             0x0, /* only sent by server */
         /* STREAM0 */               0x3,
         /* STREAM1 */               0x3,
         /* STREAM2 */               0x3,
         /* STREAM3 */               0x3,
         /* STREAM4 */               0x3,
         /* STREAM5 */               0x3,
         /* STREAM6 */               0x3,
         /* STREAM7 */               0x3,
         /* MAX_DATA */              0x3,
         /* MAX_STREAM_DATA */       0x3,
         /* MAX_STREAMS */           0x3,
         /* MAX_STREAMS2 */          0x3,
         /* DATA_BLOCKED */          0x3,
         /* STREAM_DATA_BLOCKED */   0x3,
         /* STREAMS_BLOCKED */       0x3,
         /* STREAMS_BLOCKED2 */      0x3,
         /* NEW_CONNECTION_ID */     0x3,
         /* RETIRE_CONNECTION_ID */  0x3,
         /* PATH_CHALLENGE */        0x3,
         /* PATH_RESPONSE */         0x3,
#if (NGX_QUIC_DRAFT_VERSION >= 28)
         /* CONNECTION_CLOSE */      0xF,
         /* CONNECTION_CLOSE2 */     0x3,
#else
         /* CONNECTION_CLOSE */      0xD,
         /* CONNECTION_CLOSE2 */     0x1,
#endif
         /* HANDSHAKE_DONE */        0x0, /* only sent by server */
    };

    if (ngx_quic_long_pkt(pkt->flags)) {

        if (ngx_quic_pkt_in(pkt->flags)) {
            ptype = 8; /* initial */

        } else if (ngx_quic_pkt_hs(pkt->flags)) {
            ptype = 4; /* handshake */

        } else {
            ptype = 2; /* zero-rtt */
        }

    } else {
        ptype = 1; /* application data */
    }

    if (ptype & ngx_quic_frame_masks[frame_type]) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_INFO, pkt->log, 0,
                  "quic frame type 0x%xi is not "
                  "allowed in packet with flags 0x%xd",
                  frame_type, pkt->flags);

    return NGX_DECLINED;
}


ssize_t
ngx_quic_parse_ack_range(ngx_log_t *log, u_char *start, u_char *end,
    uint64_t *gap, uint64_t *range)
{
    u_char  *p;

    p = start;

    p = ngx_quic_parse_int(p, end, gap);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "quic failed to parse ack frame gap");
        return NGX_ERROR;
    }

    p = ngx_quic_parse_int(p, end, range);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "quic failed to parse ack frame range");
        return NGX_ERROR;
    }

    return p - start;
}


size_t
ngx_quic_create_ack_range(u_char *p, uint64_t gap, uint64_t range)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(gap);
        len += ngx_quic_varint_len(range);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, gap);
    ngx_quic_build_int(&p, range);

    return p - start;
}


ssize_t
ngx_quic_create_frame(u_char *p, ngx_quic_frame_t *f)
{
    /*
     *  QUIC-recovery, section 2:
     *
     *  Ack-eliciting Frames:  All frames other than ACK, PADDING, and
     *  CONNECTION_CLOSE are considered ack-eliciting.
     */
    f->need_ack = 1;

    switch (f->type) {
    case NGX_QUIC_FT_ACK:
        f->need_ack = 0;
        return ngx_quic_create_ack(p, &f->u.ack, f->data);

    case NGX_QUIC_FT_STOP_SENDING:
        return ngx_quic_create_stop_sending(p, &f->u.stop_sending);

    case NGX_QUIC_FT_CRYPTO:
        return ngx_quic_create_crypto(p, &f->u.crypto, f->data);

    case NGX_QUIC_FT_HANDSHAKE_DONE:
        return ngx_quic_create_hs_done(p);

    case NGX_QUIC_FT_NEW_TOKEN:
        return ngx_quic_create_new_token(p, &f->u.token);

    case NGX_QUIC_FT_STREAM0:
    case NGX_QUIC_FT_STREAM1:
    case NGX_QUIC_FT_STREAM2:
    case NGX_QUIC_FT_STREAM3:
    case NGX_QUIC_FT_STREAM4:
    case NGX_QUIC_FT_STREAM5:
    case NGX_QUIC_FT_STREAM6:
    case NGX_QUIC_FT_STREAM7:
        return ngx_quic_create_stream(p, &f->u.stream, f->data);

    case NGX_QUIC_FT_CONNECTION_CLOSE:
    case NGX_QUIC_FT_CONNECTION_CLOSE_APP:
        f->need_ack = 0;
        return ngx_quic_create_close(p, &f->u.close);

    case NGX_QUIC_FT_MAX_STREAMS:
        return ngx_quic_create_max_streams(p, &f->u.max_streams);

    case NGX_QUIC_FT_MAX_STREAM_DATA:
        return ngx_quic_create_max_stream_data(p, &f->u.max_stream_data);

    case NGX_QUIC_FT_MAX_DATA:
        return ngx_quic_create_max_data(p, &f->u.max_data);

    case NGX_QUIC_FT_PATH_RESPONSE:
        return ngx_quic_create_path_response(p, &f->u.path_response);

    case NGX_QUIC_FT_NEW_CONNECTION_ID:
        return ngx_quic_create_new_connection_id(p, &f->u.ncid);

    case NGX_QUIC_FT_RETIRE_CONNECTION_ID:
        return ngx_quic_create_retire_connection_id(p, &f->u.retire_cid);

    default:
        /* BUG: unsupported frame type generated */
        return NGX_ERROR;
    }
}


static size_t
ngx_quic_create_ack(u_char *p, ngx_quic_ack_frame_t *ack, ngx_chain_t *ranges)
{
    size_t      len;
    u_char     *start;
    ngx_buf_t  *b;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_ACK);
        len += ngx_quic_varint_len(ack->largest);
        len += ngx_quic_varint_len(ack->delay);
        len += ngx_quic_varint_len(ack->range_count);
        len += ngx_quic_varint_len(ack->first_range);
        len += ack->ranges_length;

        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_ACK);
    ngx_quic_build_int(&p, ack->largest);
    ngx_quic_build_int(&p, ack->delay);
    ngx_quic_build_int(&p, ack->range_count);
    ngx_quic_build_int(&p, ack->first_range);

    while (ranges) {
        b = ranges->buf;
        p = ngx_cpymem(p, b->pos, b->last - b->pos);
        ranges = ranges->next;
    }

    return p - start;
}


static size_t
ngx_quic_create_stop_sending(u_char *p, ngx_quic_stop_sending_frame_t *ss)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_STOP_SENDING);
        len += ngx_quic_varint_len(ss->id);
        len += ngx_quic_varint_len(ss->error_code);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_STOP_SENDING);
    ngx_quic_build_int(&p, ss->id);
    ngx_quic_build_int(&p, ss->error_code);

    return p - start;
}


static size_t
ngx_quic_create_crypto(u_char *p, ngx_quic_crypto_frame_t *crypto,
    ngx_chain_t *data)
{
    size_t      len;
    u_char     *start;
    ngx_buf_t  *b;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_CRYPTO);
        len += ngx_quic_varint_len(crypto->offset);
        len += ngx_quic_varint_len(crypto->length);
        len += crypto->length;

        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_CRYPTO);
    ngx_quic_build_int(&p, crypto->offset);
    ngx_quic_build_int(&p, crypto->length);

    while (data) {
        b = data->buf;
        p = ngx_cpymem(p, b->pos, b->last - b->pos);
        data = data->next;
    }

    return p - start;
}


static size_t
ngx_quic_create_hs_done(u_char *p)
{
    u_char  *start;

    if (p == NULL) {
        return ngx_quic_varint_len(NGX_QUIC_FT_HANDSHAKE_DONE);
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_HANDSHAKE_DONE);

    return p - start;
}


static size_t
ngx_quic_create_new_token(u_char *p, ngx_quic_new_token_frame_t *token)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_NEW_TOKEN);
        len += ngx_quic_varint_len(token->length);
        len += token->length;

        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_NEW_TOKEN);
    ngx_quic_build_int(&p, token->length);
    p = ngx_cpymem(p, token->data, token->length);

    return p - start;
}


static size_t
ngx_quic_create_stream(u_char *p, ngx_quic_stream_frame_t *sf,
    ngx_chain_t *data)
{
    size_t      len;
    u_char     *start;
    ngx_buf_t  *b;

    if (p == NULL) {
        len = ngx_quic_varint_len(sf->type);

        if (sf->off) {
            len += ngx_quic_varint_len(sf->offset);
        }

        len += ngx_quic_varint_len(sf->stream_id);

        /* length is always present in generated frames */
        len += ngx_quic_varint_len(sf->length);

        len += sf->length;

        return len;
    }

    start = p;

    ngx_quic_build_int(&p, sf->type);
    ngx_quic_build_int(&p, sf->stream_id);

    if (sf->off) {
        ngx_quic_build_int(&p, sf->offset);
    }

    /* length is always present in generated frames */
    ngx_quic_build_int(&p, sf->length);

    while (data) {
        b = data->buf;
        p = ngx_cpymem(p, b->pos, b->last - b->pos);
        data = data->next;
    }

    return p - start;
}


static size_t
ngx_quic_create_max_streams(u_char *p, ngx_quic_max_streams_frame_t *ms)
{
    size_t       len;
    u_char      *start;
    ngx_uint_t   type;

    type = ms->bidi ?  NGX_QUIC_FT_MAX_STREAMS : NGX_QUIC_FT_MAX_STREAMS2;

    if (p == NULL) {
        len = ngx_quic_varint_len(type);
        len += ngx_quic_varint_len(ms->limit);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, type);
    ngx_quic_build_int(&p, ms->limit);

    return p - start;
}


static ngx_int_t
ngx_quic_parse_transport_param(u_char *p, u_char *end, uint16_t id,
    ngx_quic_tp_t *dst)
{
    uint64_t   varint;
    ngx_str_t  str;

    varint = 0;
    ngx_str_null(&str);

    switch (id) {

    case NGX_QUIC_TP_DISABLE_ACTIVE_MIGRATION:
        /* zero-length option */
        if (end - p != 0) {
            return NGX_ERROR;
        }
        dst->disable_active_migration = 1;
        return NGX_OK;

    case NGX_QUIC_TP_MAX_IDLE_TIMEOUT:
    case NGX_QUIC_TP_MAX_UDP_PAYLOAD_SIZE:
    case NGX_QUIC_TP_INITIAL_MAX_DATA:
    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL:
    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE:
    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_UNI:
    case NGX_QUIC_TP_INITIAL_MAX_STREAMS_BIDI:
    case NGX_QUIC_TP_INITIAL_MAX_STREAMS_UNI:
    case NGX_QUIC_TP_ACK_DELAY_EXPONENT:
    case NGX_QUIC_TP_MAX_ACK_DELAY:
    case NGX_QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT:

        p = ngx_quic_parse_int(p, end, &varint);
        if (p == NULL) {
            return NGX_ERROR;
        }
        break;

    case NGX_QUIC_TP_INITIAL_SCID:

        str.len = end - p;
        str.data = p;
        break;

    default:
        return NGX_DECLINED;
    }

    switch (id) {

    case NGX_QUIC_TP_MAX_IDLE_TIMEOUT:
        dst->max_idle_timeout = varint;
        break;

    case NGX_QUIC_TP_MAX_UDP_PAYLOAD_SIZE:
        dst->max_udp_payload_size = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_DATA:
        dst->initial_max_data = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL:
        dst->initial_max_stream_data_bidi_local = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE:
        dst->initial_max_stream_data_bidi_remote = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_UNI:
        dst->initial_max_stream_data_uni = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_STREAMS_BIDI:
        dst->initial_max_streams_bidi = varint;
        break;

    case NGX_QUIC_TP_INITIAL_MAX_STREAMS_UNI:
        dst->initial_max_streams_uni = varint;
        break;

    case NGX_QUIC_TP_ACK_DELAY_EXPONENT:
        dst->ack_delay_exponent = varint;
        break;

    case NGX_QUIC_TP_MAX_ACK_DELAY:
        dst->max_ack_delay = varint;
        break;

    case NGX_QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT:
        dst->active_connection_id_limit = varint;
        break;

    case NGX_QUIC_TP_INITIAL_SCID:
        dst->initial_scid = str;
        break;

    default:
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_quic_parse_transport_params(u_char *p, u_char *end, ngx_quic_tp_t *tp,
    ngx_log_t *log)
{
    uint64_t   id, len;
    ngx_int_t  rc;

    while (p < end) {
        p = ngx_quic_parse_int(p, end, &id);
        if (p == NULL) {
            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "quic failed to parse transport param id");
            return NGX_ERROR;
        }

        switch (id) {
        case NGX_QUIC_TP_ORIGINAL_DCID:
        case NGX_QUIC_TP_PREFERRED_ADDRESS:
        case NGX_QUIC_TP_RETRY_SCID:
        case NGX_QUIC_TP_SR_TOKEN:
            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "quic client sent forbidden transport param"
                          " id:0x%xL", id);
            return NGX_ERROR;
        }

        p = ngx_quic_parse_int(p, end, &len);
        if (p == NULL) {
            ngx_log_error(NGX_LOG_INFO, log, 0,
                         "quic failed to parse"
                         " transport param id:0x%xL length", id);
            return NGX_ERROR;
        }

        rc = ngx_quic_parse_transport_param(p, p + len, id, tp);

        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "quic failed to parse"
                          " transport param id:0x%xL data", id);
            return NGX_ERROR;
        }

        if (rc == NGX_DECLINED) {
            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "quic unknown transport param id:0x%xL, skipped", id);
        }

        p += len;
    }

    if (p != end) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "quic trailing garbage in"
                      " transport parameters: bytes:%ui",
                      end - p);
        return NGX_ERROR;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic transport parameters parsed ok");

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp disable active migration: %ui",
                   tp->disable_active_migration);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0, "quic tp idle_timeout:%ui",
                   tp->max_idle_timeout);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp max_udp_payload_size:%ui",
                   tp->max_udp_payload_size);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0, "quic tp max_data:%ui",
                   tp->initial_max_data);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp max_stream_data_bidi_local:%ui",
                   tp->initial_max_stream_data_bidi_local);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp max_stream_data_bidi_remote:%ui",
                   tp->initial_max_stream_data_bidi_remote);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp max_stream_data_uni:%ui",
                   tp->initial_max_stream_data_uni);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp initial_max_streams_bidi:%ui",
                   tp->initial_max_streams_bidi);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp initial_max_streams_uni:%ui",
                   tp->initial_max_streams_uni);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp ack_delay_exponent:%ui",
                   tp->ack_delay_exponent);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0, "quic tp max_ack_delay:%ui",
                   tp->max_ack_delay);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp active_connection_id_limit:%ui",
                   tp->active_connection_id_limit);

#if (NGX_QUIC_DRAFT_VERSION >= 28)
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, log, 0,
                   "quic tp initial source_connection_id len:%uz %xV",
                   tp->initial_scid.len, &tp->initial_scid);
#endif

    return NGX_OK;
}


static size_t
ngx_quic_create_max_stream_data(u_char *p, ngx_quic_max_stream_data_frame_t *ms)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_MAX_STREAM_DATA);
        len += ngx_quic_varint_len(ms->id);
        len += ngx_quic_varint_len(ms->limit);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_MAX_STREAM_DATA);
    ngx_quic_build_int(&p, ms->id);
    ngx_quic_build_int(&p, ms->limit);

    return p - start;
}


static size_t
ngx_quic_create_max_data(u_char *p, ngx_quic_max_data_frame_t *md)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_MAX_DATA);
        len += ngx_quic_varint_len(md->max_data);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_MAX_DATA);
    ngx_quic_build_int(&p, md->max_data);

    return p - start;
}


static size_t
ngx_quic_create_path_response(u_char *p, ngx_quic_path_challenge_frame_t *pc)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_PATH_RESPONSE);
        len += sizeof(pc->data);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_PATH_RESPONSE);
    p = ngx_cpymem(p, &pc->data, sizeof(pc->data));

    return p - start;
}


static size_t
ngx_quic_create_new_connection_id(u_char *p, ngx_quic_new_conn_id_frame_t *ncid)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_NEW_CONNECTION_ID);
        len += ngx_quic_varint_len(ncid->seqnum);
        len += ngx_quic_varint_len(ncid->retire);
        len++;
        len += ncid->len;
        len += NGX_QUIC_SR_TOKEN_LEN;
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_NEW_CONNECTION_ID);
    ngx_quic_build_int(&p, ncid->seqnum);
    ngx_quic_build_int(&p, ncid->retire);
    *p++ = ncid->len;
    p = ngx_cpymem(p, ncid->cid, ncid->len);
    p = ngx_cpymem(p, ncid->srt, NGX_QUIC_SR_TOKEN_LEN);

    return p - start;
}


static size_t
ngx_quic_create_retire_connection_id(u_char *p,
    ngx_quic_retire_cid_frame_t *rcid)
{
    size_t   len;
    u_char  *start;

    if (p == NULL) {
        len = ngx_quic_varint_len(NGX_QUIC_FT_RETIRE_CONNECTION_ID);
        len += ngx_quic_varint_len(rcid->sequence_number);
        return len;
    }

    start = p;

    ngx_quic_build_int(&p, NGX_QUIC_FT_RETIRE_CONNECTION_ID);
    ngx_quic_build_int(&p, rcid->sequence_number);

    return p - start;
}


ssize_t
ngx_quic_create_transport_params(u_char *pos, u_char *end, ngx_quic_tp_t *tp,
    size_t *clen)
{
    u_char  *p;
    size_t   len;

#define ngx_quic_tp_len(id, value)                                            \
    ngx_quic_varint_len(id)                                                   \
    + ngx_quic_varint_len(value)                                              \
    + ngx_quic_varint_len(ngx_quic_varint_len(value))

#define ngx_quic_tp_vint(id, value)                                           \
    do {                                                                      \
        ngx_quic_build_int(&p, id);                                           \
        ngx_quic_build_int(&p, ngx_quic_varint_len(value));                   \
        ngx_quic_build_int(&p, value);                                        \
    } while (0)

#define ngx_quic_tp_strlen(id, value)                                         \
    ngx_quic_varint_len(id)                                                   \
    + ngx_quic_varint_len(value.len)                                          \
    + value.len

#define ngx_quic_tp_str(id, value)                                            \
    do {                                                                      \
        ngx_quic_build_int(&p, id);                                           \
        ngx_quic_build_int(&p, value.len);                                    \
        p = ngx_cpymem(p, value.data, value.len);                             \
    } while (0)

    p = pos;

    len = ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_DATA, tp->initial_max_data);

    len += ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_STREAMS_UNI,
                           tp->initial_max_streams_uni);

    len += ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_STREAMS_BIDI,
                           tp->initial_max_streams_bidi);

    len += ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
                           tp->initial_max_stream_data_bidi_local);

    len += ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
                           tp->initial_max_stream_data_bidi_remote);

    len += ngx_quic_tp_len(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_UNI,
                           tp->initial_max_stream_data_uni);

    len += ngx_quic_tp_len(NGX_QUIC_TP_MAX_IDLE_TIMEOUT,
                           tp->max_idle_timeout);

    if (clen) {
        *clen = len;
    }

    if (tp->disable_active_migration) {
        len += ngx_quic_varint_len(NGX_QUIC_TP_DISABLE_ACTIVE_MIGRATION);
        len += ngx_quic_varint_len(0);
    }

    len += ngx_quic_tp_len(NGX_QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT,
                           tp->active_connection_id_limit);

#if (NGX_QUIC_DRAFT_VERSION >= 28)
    len += ngx_quic_tp_strlen(NGX_QUIC_TP_ORIGINAL_DCID, tp->original_dcid);
    len += ngx_quic_tp_strlen(NGX_QUIC_TP_INITIAL_SCID, tp->initial_scid);

    if (tp->retry_scid.len) {
        len += ngx_quic_tp_strlen(NGX_QUIC_TP_RETRY_SCID, tp->retry_scid);
    }
#else
    if (tp->original_dcid.len) {
        len += ngx_quic_tp_strlen(NGX_QUIC_TP_ORIGINAL_DCID, tp->original_dcid);
    }
#endif

    len += ngx_quic_varint_len(NGX_QUIC_TP_SR_TOKEN);
    len += ngx_quic_varint_len(NGX_QUIC_SR_TOKEN_LEN);
    len += NGX_QUIC_SR_TOKEN_LEN;

    if (pos == NULL) {
        return len;
    }

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_DATA,
                     tp->initial_max_data);

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_STREAMS_UNI,
                     tp->initial_max_streams_uni);

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_STREAMS_BIDI,
                     tp->initial_max_streams_bidi);

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
                     tp->initial_max_stream_data_bidi_local);

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
                     tp->initial_max_stream_data_bidi_remote);

    ngx_quic_tp_vint(NGX_QUIC_TP_INITIAL_MAX_STREAM_DATA_UNI,
                     tp->initial_max_stream_data_uni);

    ngx_quic_tp_vint(NGX_QUIC_TP_MAX_IDLE_TIMEOUT,
                     tp->max_idle_timeout);

    if (tp->disable_active_migration) {
        ngx_quic_build_int(&p, NGX_QUIC_TP_DISABLE_ACTIVE_MIGRATION);
        ngx_quic_build_int(&p, 0);
    }

    ngx_quic_tp_vint(NGX_QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT,
                     tp->active_connection_id_limit);

#if (NGX_QUIC_DRAFT_VERSION >= 28)
    ngx_quic_tp_str(NGX_QUIC_TP_ORIGINAL_DCID, tp->original_dcid);
    ngx_quic_tp_str(NGX_QUIC_TP_INITIAL_SCID, tp->initial_scid);

    if (tp->retry_scid.len) {
        ngx_quic_tp_str(NGX_QUIC_TP_RETRY_SCID, tp->retry_scid);
    }
#else
    if (tp->original_dcid.len) {
        ngx_quic_tp_str(NGX_QUIC_TP_ORIGINAL_DCID, tp->original_dcid);
    }
#endif

    ngx_quic_build_int(&p, NGX_QUIC_TP_SR_TOKEN);
    ngx_quic_build_int(&p, NGX_QUIC_SR_TOKEN_LEN);
    p = ngx_cpymem(p, tp->sr_token, NGX_QUIC_SR_TOKEN_LEN);

    return p - pos;
}


static size_t
ngx_quic_create_close(u_char *p, ngx_quic_close_frame_t *cl)
{
    size_t       len;
    u_char      *start;
    ngx_uint_t   type;

    type = cl->app ? NGX_QUIC_FT_CONNECTION_CLOSE_APP
                   : NGX_QUIC_FT_CONNECTION_CLOSE;

    if (p == NULL) {
        len = ngx_quic_varint_len(type);
        len += ngx_quic_varint_len(cl->error_code);

        if (!cl->app) {
            len += ngx_quic_varint_len(cl->frame_type);
        }

        len += ngx_quic_varint_len(cl->reason.len);
        len += cl->reason.len;

        return len;
    }

    start = p;

    ngx_quic_build_int(&p, type);
    ngx_quic_build_int(&p, cl->error_code);

    if (!cl->app) {
        ngx_quic_build_int(&p, cl->frame_type);
    }

    ngx_quic_build_int(&p, cl->reason.len);
    p = ngx_cpymem(p, cl->reason.data, cl->reason.len);

    return p - start;
}


void
ngx_quic_dcid_encode_key(u_char *dcid, uint64_t key)
{
    (void) ngx_quic_write_uint64(dcid, key);
}