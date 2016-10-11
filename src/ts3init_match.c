/*
 *    "ts3init" extension for Xtables
 *
 *    Description: A module to aid in ts3 spoof protection
 *                 This is the "match" code
 *
 *    Authors:
 *    Niels Werensteijn <niels werensteijn [at] teamspeak com>, 2016-10-03
 *
 *    This program is free software; you can redistribute it and/or modify it
 *    under the terms of the GNU General Public License; either version 2
 *    or 3 of the License, as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/netfilter/x_tables.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/percpu.h>
#include "ts3init_match.h"
#include "ts3init_cookie.h"
#include "ts3init_header.h"

struct ts3init_cache_t
{
    unsigned long                  saved_jiffies;
    time_t                         unix_time;
    struct xt_ts3init_cookie_cache cookie_cache;
};        

static const struct ts3_init_header_tag ts3init_header_tag_signature =
    { .tag8 = {'T', 'S', '3', 'I', 'N', 'I', 'T', '1'} };

static const int header_size = 18;
static int payload_sizes[] = { 16, 20, 20, 244, -1, 1 };
	
DEFINE_PER_CPU(struct ts3init_cache_t, ts3init_cache);

bool check_header(const struct sk_buff *skb, const struct xt_action_param *par,
    struct ts3_init_checked_header_data* header_data)
{
    unsigned int data_len;
    struct udphdr *udp;
    struct ts3_init_header* ts3_header;
    int expected_payload_size;
    
    udp = skb_header_pointer(skb, par->thoff, sizeof(*udp), &header_data->udp_buf);
    data_len = be16_to_cpu(udp->len) - sizeof(*udp);

    if (data_len < header_size) return false;

    ts3_header = (struct ts3_init_header*) skb_header_pointer(skb, 
        par->thoff + sizeof(*udp), data_len,
        &header_data->ts3_header_buf);

    if (!ts3_header) return false;

    if (ts3_header->tag.tag64 != ts3init_header_tag_signature.tag64) return false;
    if (ts3_header->packet_id != cpu_to_be16(101)) return false;
    if (ts3_header->client_id != 0) return false;
    if (ts3_header->flags != 0x88) return false;
	if (ts3_header->command >= COMMAND_MAX) return false;

    /* TODO: check min_client_version if needed */
    	
	/* TODO: add payload size check for COMMAND_SOLVE_PUZZLE */
	expected_payload_size = payload_sizes[ts3_header->command];
    if (data_len != header_size + expected_payload_size) return false;

    header_data->udp = udp;    
    header_data->ts3_header = ts3_header;
    return true;
}

static inline void update_cache_time(unsigned long jifs,
    struct ts3init_cache_t* cache)
{
    if (((long)jifs - (long)cache->saved_jiffies) >= HZ)
    {
        /* it's been 1 second sinds last time update.
         * Get the new unix time and cache it*/
       struct timeval tv;
       cache->saved_jiffies = jifs;
       do_gettimeofday(&tv);
       cache->unix_time = tv.tv_sec;
   }
}

static bool
ts3init_get_cookie_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
    const struct xt_ts3init_get_cookie_mtinfo *info = par->matchinfo;
    struct ts3_init_checked_header_data header_data;
    
    if (!check_header(skb, par, &header_data))
        return false;
        
    if (header_data.ts3_header->command != COMMAND_GET_COOKIE) return false;
        
    if (info->specific_options & CHK_GET_COOKIE_CHECK_TIMESTAMP)
    {
        struct ts3init_cache_t* cache;
        unsigned long jifs;
        time_t current_unix_time, packet_unix_time;

        jifs = jiffies;
                
        cache = &get_cpu_var(ts3init_cache);
                
        update_cache_time(jifs, cache);
                
        current_unix_time = cache->unix_time;
                
        put_cpu_var(ts3init_cache);

        packet_unix_time =
            header_data.ts3_header->payload[0] << 24 |
            header_data.ts3_header->payload[1] << 16 |
            header_data.ts3_header->payload[2] << 8  |
            header_data.ts3_header->payload[3];

        if (abs(current_unix_time - packet_unix_time) > info->max_utc_offset)
            return false;
    }
    return true;
}

static int ts3init_get_cookie_mt_check(const struct xt_mtchk_param *par)
{
    struct xt_ts3init_get_cookie_mtinfo *info = par->matchinfo;

    if (! (par->family == NFPROTO_IPV4 || par->family == NFPROTO_IPV6))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid protocol (only ipv4 and ipv6) for get_cookie\n");
        return -EINVAL;
    }

    if (info->common_options & ~(CHK_COMMON_VALID_MASK))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid (common) options for get_cookie\n");
        return -EINVAL;
    }

    if (info->specific_options & ~(CHK_GET_COOKIE_VALID_MASK))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid (specific) options for get_cookie\n");
        return -EINVAL;
    }
    
    return 0;
}    

static bool
ts3init_get_puzzle_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
    const struct xt_ts3init_get_puzzle_mtinfo *info = par->matchinfo;
    struct ts3_init_checked_header_data header_data;

    if (!check_header(skb, par, &header_data))
        return false;

    if (header_data.ts3_header->command != COMMAND_GET_PUZZLE) return false;

    if (info->specific_options & CHK_GET_PUZZLE_CHECK_COOKIE)
    {
        struct ts3init_cache_t* cache;
        struct ts3_init_header* ts3_header = header_data.ts3_header;
        __u64* cookie_seed, cookie_seed0, cookie_seed1;
        __u64 cookie, packet_cookie;

        unsigned long jifs;
        time_t current_unix_time;

        jifs = jiffies;
        cache = &get_cpu_var(ts3init_cache);

        update_cache_time(jifs, cache);

        current_unix_time = cache->unix_time;

        cookie_seed = ts3init_get_cookie_seed(current_unix_time,
            ts3_header->payload[8], &cache->cookie_cache,
            info->cookie_seed);

        if (!cookie_seed)
        {
            put_cpu_var(ts3init_cache);
            return false;
        }

        cookie_seed0 = cookie_seed[0];
        cookie_seed1 = cookie_seed[1];

        put_cpu_var(ts3init_cache);

        /* use cookie_seed and ipaddress and port to create a hash
         * (cookie) for this connection */
        if (ts3init_calculate_cookie(skb, par, header_data.udp, cookie_seed0, cookie_seed1, &cookie))
            return false; /*something went wrong*/

        /* compare cookie with payload bytes 0-7. if equal, cookie
         * is valid */
         
        packet_cookie = (((u64)((ts3_header->payload)[0])) | ((u64)((ts3_header->payload)[1]) << 8) |
           ((u64)((ts3_header->payload)[2]) << 16) | ((u64)((ts3_header->payload)[3]) << 24) |
           ((u64)((ts3_header->payload)[4]) << 32) | ((u64)((ts3_header->payload)[5]) << 40) |
           ((u64)((ts3_header->payload)[6]) << 48) | ((u64)((ts3_header->payload)[7]) << 56));
           
        if (packet_cookie != cookie) return false;
    }
    return true;
}

static int ts3init_get_puzzle_mt_check(const struct xt_mtchk_param *par)
{
    struct xt_ts3init_get_puzzle_mtinfo *info = par->matchinfo;
    
        if (! (par->family == NFPROTO_IPV4 || par->family == NFPROTO_IPV6))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid protocol (only ipv4 and ipv6) for get_puzzle\n");
        return -EINVAL;
    }

    if (info->common_options & ~(CHK_COMMON_VALID_MASK))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid (common) options for get_puzzle\n");
        return -EINVAL;
    }

    if (info->specific_options & ~(CHK_GET_PUZZLE_VALID_MASK))
    {
        printk(KERN_INFO KBUILD_MODNAME ": invalid (specific) options for get_cookie\n");
        return -EINVAL;
    }
    
    return 0;
}    


static struct xt_match ts3init_mt_reg[] __read_mostly = {
    {
        .name       = "ts3init_get_cookie",
        .revision   = 0,
        .family     = NFPROTO_IPV4,
        .proto      = IPPROTO_UDP,
        .matchsize  = sizeof(struct xt_ts3init_get_cookie_mtinfo),
        .match      = ts3init_get_cookie_mt,
        .checkentry = ts3init_get_cookie_mt_check,
        .me         = THIS_MODULE,
    },
    {
        .name       = "ts3init_get_cookie",
        .revision   = 0,
        .family     = NFPROTO_IPV6,
        .proto      = IPPROTO_UDP,
        .matchsize  = sizeof(struct xt_ts3init_get_cookie_mtinfo),
        .match      = ts3init_get_cookie_mt,
        .checkentry = ts3init_get_cookie_mt_check,
        .me         = THIS_MODULE,
    },
    {
        .name       = "ts3init_get_puzzle",
        .revision   = 0,
        .family     = NFPROTO_IPV4,
        .proto      = IPPROTO_UDP,
        .matchsize  = sizeof(struct xt_ts3init_get_puzzle_mtinfo),
        .match      = ts3init_get_puzzle_mt,
        .checkentry = ts3init_get_puzzle_mt_check,
        .me         = THIS_MODULE,
    },
    {
        .name       = "ts3init_get_puzzle",
        .revision   = 0,
        .family     = NFPROTO_IPV6,
        .proto      = IPPROTO_UDP,
        .matchsize  = sizeof(struct xt_ts3init_get_puzzle_mtinfo),
        .match      = ts3init_get_puzzle_mt,
        .checkentry = ts3init_get_puzzle_mt_check,
        .me         = THIS_MODULE,
    },
};

int __init ts3init_match_init(void)
{
    return xt_register_matches(ts3init_mt_reg, ARRAY_SIZE(ts3init_mt_reg));
}

void __exit ts3init_match_exit(void)
{
    xt_unregister_matches(ts3init_mt_reg, ARRAY_SIZE(ts3init_mt_reg));
}