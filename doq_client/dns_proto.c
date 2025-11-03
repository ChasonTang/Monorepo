/**
 * DNS Protocol Implementation for DoQ Client
 */

#include "dns_proto.h"
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

/**
 * Encode a domain name in DNS label format
 * e.g., "google.com" -> "\x06google\x03com\x00"
 */
static int dns_encode_domain(const char *domain, uint8_t *buffer, size_t bufsize) {
    const char *p = domain;
    uint8_t *len_ptr = buffer;
    uint8_t *dst = buffer + 1;
    uint8_t label_len = 0;
    size_t total_len = 0;

    if (bufsize < strlen(domain) + 2) {
        return -1;
    }

    while (*p) {
        if (*p == '.') {
            if (label_len == 0 || label_len > DNS_MAX_LABEL_SIZE) {
                return -1;
            }
            *len_ptr = label_len;
            len_ptr = dst;
            dst++;
            label_len = 0;
            total_len++;
        } else {
            *dst++ = *p;
            label_len++;
            total_len++;
        }
        p++;
    }

    /* Write the last label length */
    if (label_len > 0) {
        if (label_len > DNS_MAX_LABEL_SIZE) {
            return -1;
        }
        *len_ptr = label_len;
        total_len++;
    }

    /* Null terminator */
    *dst = 0;
    total_len++;

    return total_len;
}

/**
 * Build a DNS query message for an A record lookup
 */
int dns_build_query(const char *domain, uint8_t *buffer, size_t bufsize, uint16_t trans_id) {
    dns_header_t header;
    uint8_t *p = buffer;
    int domain_len;
    uint16_t qtype, qclass;

    if (bufsize < sizeof(dns_header_t) + DNS_MAX_DOMAIN_SIZE + 4) {
        return -1;
    }

    /* Build DNS header */
    memset(&header, 0, sizeof(header));
    header.id = htons(trans_id);
    header.flags = htons(DNS_FLAG_RD);  /* Standard query with recursion desired */
    header.qdcount = htons(1);          /* One question */
    header.ancount = 0;
    header.nscount = 0;
    header.arcount = 0;

    /* Write header to buffer */
    memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    /* Encode domain name */
    domain_len = dns_encode_domain(domain, p, bufsize - sizeof(header) - 4);
    if (domain_len < 0) {
        return -1;
    }
    p += domain_len;

    /* Write QTYPE (A record = 1) */
    qtype = htons(DNS_TYPE_A);
    memcpy(p, &qtype, sizeof(qtype));
    p += sizeof(qtype);

    /* Write QCLASS (IN = 1) */
    qclass = htons(DNS_CLASS_IN);
    memcpy(p, &qclass, sizeof(qclass));
    p += sizeof(qclass);

    return p - buffer;
}

/**
 * Parse a DNS response message
 */
int dns_parse_response(const uint8_t *buffer, size_t bufsize, dns_header_t *header) {
    if (bufsize < sizeof(dns_header_t)) {
        return -1;
    }

    /* Parse header */
    memcpy(header, buffer, sizeof(dns_header_t));
    
    /* Convert from network byte order */
    header->id = ntohs(header->id);
    header->flags = ntohs(header->flags);
    header->qdcount = ntohs(header->qdcount);
    header->ancount = ntohs(header->ancount);
    header->nscount = ntohs(header->nscount);
    header->arcount = ntohs(header->arcount);

    /* Check if it's a response */
    if (!(header->flags & DNS_FLAG_QR)) {
        fprintf(stderr, "Not a DNS response\n");
        return -1;
    }

    /* Check response code */
    int rcode = header->flags & DNS_FLAG_RCODE;
    if (rcode != DNS_RCODE_NOERROR) {
        fprintf(stderr, "DNS error: rcode=%d\n", rcode);
        return -1;
    }

    return 0;
}

/**
 * Skip a DNS name in the message (handles compression)
 */
static int dns_skip_name(const uint8_t *buffer, size_t bufsize, size_t offset) {
    size_t pos = offset;
    int jumps = 0;
    const int MAX_JUMPS = 5;

    while (pos < bufsize) {
        uint8_t len = buffer[pos];

        /* Check for compression pointer (11xxxxxx xxxxxxxx) */
        if ((len & 0xC0) == 0xC0) {
            if (jumps == 0) {
                /* First jump, advance position by 2 bytes */
                return pos - offset + 2;
            }
            /* Follow the pointer */
            if (pos + 1 >= bufsize) {
                return -1;
            }
            uint16_t ptr = ((len & 0x3F) << 8) | buffer[pos + 1];
            pos = ptr;
            jumps++;
            if (jumps > MAX_JUMPS) {
                return -1;  /* Avoid infinite loops */
            }
            continue;
        }

        /* Regular label */
        if (len == 0) {
            /* End of name */
            return pos - offset + 1;
        }

        /* Move to next label */
        pos += len + 1;
    }

    return -1;  /* Name extends beyond buffer */
}

/**
 * Extract IPv4 addresses from a DNS response
 */
int dns_extract_a_records(const uint8_t *buffer, size_t bufsize,
                          char addrs[][16], int max_addrs) {
    dns_header_t header;
    size_t offset;
    int addr_count = 0;
    int i;

    if (dns_parse_response(buffer, bufsize, &header) < 0) {
        return -1;
    }

    if (header.ancount == 0) {
        fprintf(stderr, "No answers in DNS response\n");
        return 0;
    }

    /* Skip header */
    offset = sizeof(dns_header_t);

    /* Skip questions section */
    for (i = 0; i < header.qdcount; i++) {
        int skip = dns_skip_name(buffer, bufsize, offset);
        if (skip < 0) {
            return -1;
        }
        offset += skip;
        offset += 4;  /* QTYPE + QCLASS */
        
        if (offset > bufsize) {
            return -1;
        }
    }

    /* Parse answer section */
    for (i = 0; i < header.ancount && addr_count < max_addrs; i++) {
        uint16_t type, class, rdlength;
        uint32_t ttl;

        /* Skip name */
        int skip = dns_skip_name(buffer, bufsize, offset);
        if (skip < 0) {
            return -1;
        }
        offset += skip;

        /* Check if we have enough bytes for the record */
        if (offset + 10 > bufsize) {
            return -1;
        }

        /* Read TYPE */
        memcpy(&type, buffer + offset, 2);
        type = ntohs(type);
        offset += 2;

        /* Read CLASS */
        memcpy(&class, buffer + offset, 2);
        class = ntohs(class);
        offset += 2;

        /* Read TTL */
        memcpy(&ttl, buffer + offset, 4);
        ttl = ntohl(ttl);
        offset += 4;

        /* Read RDLENGTH */
        memcpy(&rdlength, buffer + offset, 2);
        rdlength = ntohs(rdlength);
        offset += 2;

        /* Check if RDATA fits in buffer */
        if (offset + rdlength > bufsize) {
            return -1;
        }

        /* If it's an A record, extract the IP address */
        if (type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlength == 4) {
            uint8_t ip[4];
            memcpy(ip, buffer + offset, 4);
            snprintf(addrs[addr_count], 16, "%u.%u.%u.%u",
                     ip[0], ip[1], ip[2], ip[3]);
            addr_count++;
        }

        /* Move to next record */
        offset += rdlength;
    }

    return addr_count;
}

