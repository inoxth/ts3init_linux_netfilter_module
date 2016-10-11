#ifndef _TS3INIT_COOKIE_H
#define _TS3INIT_COOKIE_H

enum {
    SHA512_SIZE = 64,
    SIP_KEY_SIZE = 16
};

struct xt_ts3init_cookie_cache
{
    time_t time[2];
    union
    {
        __u8 seed8[SHA512_SIZE*2];
        __u64 seed64[(SHA512_SIZE/sizeof(__u64))*2];
    };
};

__u64* ts3init_get_cookie_seed(time_t current_time, __u8 packet_index, 
                struct xt_ts3init_cookie_cache* cache,
                const __u8* cookie_seed);

int ts3init_calculate_cookie(const struct sk_buff *skb,
                struct xt_action_param *par, struct udphdr *udp,
                u64 k0, u64 k1, __u64* out);
                
#endif /* _TS3INIT_COOKIE_H */