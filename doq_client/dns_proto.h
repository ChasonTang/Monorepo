/**
 * DNS Protocol Implementation for DoQ Client
 * RFC 1035 - Domain Names - Implementation and Specification
 */

#ifndef DNS_PROTO_H
#define DNS_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* DNS Header Flags */
#define DNS_FLAG_QR     0x8000  /* Query/Response flag */
#define DNS_FLAG_OPCODE 0x7800  /* Operation code */
#define DNS_FLAG_AA     0x0400  /* Authoritative Answer */
#define DNS_FLAG_TC     0x0200  /* Truncation */
#define DNS_FLAG_RD     0x0100  /* Recursion Desired */
#define DNS_FLAG_RA     0x0080  /* Recursion Available */
#define DNS_FLAG_Z      0x0070  /* Reserved */
#define DNS_FLAG_RCODE  0x000F  /* Response code */

/* DNS Query Types */
#define DNS_TYPE_A      1       /* IPv4 address */
#define DNS_TYPE_NS     2       /* Name server */
#define DNS_TYPE_CNAME  5       /* Canonical name */
#define DNS_TYPE_SOA    6       /* Start of authority */
#define DNS_TYPE_PTR    12      /* Pointer */
#define DNS_TYPE_MX     15      /* Mail exchange */
#define DNS_TYPE_TXT    16      /* Text */
#define DNS_TYPE_AAAA   28      /* IPv6 address */

/* DNS Query Classes */
#define DNS_CLASS_IN    1       /* Internet */

/* DNS Response Codes */
#define DNS_RCODE_NOERROR   0
#define DNS_RCODE_FORMERR   1
#define DNS_RCODE_SERVFAIL  2
#define DNS_RCODE_NXDOMAIN  3
#define DNS_RCODE_NOTIMP    4
#define DNS_RCODE_REFUSED   5

/* Maximum DNS message size */
#define DNS_MAX_MESSAGE_SIZE    512
#define DNS_MAX_LABEL_SIZE      63
#define DNS_MAX_DOMAIN_SIZE     255

/* DNS Header (12 bytes) */
typedef struct {
    uint16_t id;        /* Transaction ID */
    uint16_t flags;     /* Flags */
    uint16_t qdcount;   /* Number of questions */
    uint16_t ancount;   /* Number of answers */
    uint16_t nscount;   /* Number of authority records */
    uint16_t arcount;   /* Number of additional records */
} dns_header_t;

/* DNS Question */
typedef struct {
    char *qname;        /* Domain name */
    uint16_t qtype;     /* Query type */
    uint16_t qclass;    /* Query class */
} dns_question_t;

/* DNS Resource Record */
typedef struct {
    char *name;         /* Domain name */
    uint16_t type;      /* Record type */
    uint16_t class;     /* Record class */
    uint32_t ttl;       /* Time to live */
    uint16_t rdlength;  /* Resource data length */
    uint8_t *rdata;     /* Resource data */
} dns_rr_t;

/**
 * Build a DNS query message for an A record lookup
 * @param domain    Domain name to query (e.g., "google.com")
 * @param buffer    Output buffer for the DNS message
 * @param bufsize   Size of the output buffer
 * @param trans_id  Transaction ID (random value)
 * @return          Length of the DNS message, or -1 on error
 */
int dns_build_query(const char *domain, uint8_t *buffer, size_t bufsize, uint16_t trans_id);

/**
 * Parse a DNS response message
 * @param buffer    Buffer containing the DNS response
 * @param bufsize   Size of the buffer
 * @param header    Output: DNS header
 * @return          0 on success, -1 on error
 */
int dns_parse_response(const uint8_t *buffer, size_t bufsize, dns_header_t *header);

/**
 * Extract IPv4 addresses from a DNS response
 * @param buffer    Buffer containing the DNS response
 * @param bufsize   Size of the buffer
 * @param addrs     Output buffer for IPv4 addresses (as strings)
 * @param max_addrs Maximum number of addresses to extract
 * @return          Number of addresses extracted, or -1 on error
 */
int dns_extract_a_records(const uint8_t *buffer, size_t bufsize, 
                          char addrs[][16], int max_addrs);

#endif /* DNS_PROTO_H */

