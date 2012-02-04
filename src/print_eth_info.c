// vim:ts=8:expandtab
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_version.h>

#include "i3status.h"

#if defined(LINUX)
    #if defined(USE_PROC_NET_DEV)
        static long long prev_recv_bytes, prev_sent_bytes, 
        prev_nanoseconds = -1;
    #else
        #include <linux/ethtool.h>
    #endif
#include <linux/sockios.h>
#define PART_ETHSPEED  "E: %s (%d Mbit/s)"
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
#include <net/if_media.h>
#define IFM_TYPE_MATCH(dt, t)                       \
        (IFM_TYPE((dt)) == 0 || IFM_TYPE((dt)) == IFM_TYPE((t)))

#define PART_ETHSPEED  "E: %s (%s)"
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
#include <errno.h>
#include <net/if_media.h>
#endif

static int print_eth_speed(char *outwalk, const char *interface) {
#if defined(LINUX)
        #if defined(USE_PROC_NET_DEV)
            char buffer[4096], cad[256], *ni, *nf;
            
            // read network information
            int fd = open("/proc/net/dev", O_RDONLY);
            if (fd < 0) return;
            int bytes = read(fd, buffer, sizeof(buffer)-1);
            close(fd);
            if (bytes < 0) return;
            buffer[bytes] = 0;

            struct timespec tp;
            clock_gettime(CLOCK_MONOTONIC, &tp);
            long long nanoseconds = tp.tv_sec * 1000000000LL + tp.tv_nsec;

            long long recv_bytes=LLONG_MAX, sent_bytes=LLONG_MAX;
            int networkAvailable = 0;

            // search for the proper network interface
            strcpy(cad, interface);
            strcat(cad, ":");
            char *pif = strstr(buffer, cad);
            if (pif != NULL) {
                networkAvailable = 1;

                // jump to the received bytes field
                ni = pif + strlen(cad);
                while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
                for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
                *nf++ = 0;

                // get the received bytes
                recv_bytes = atoll(ni);

                // jump to the sent bytes field
                        int skip;
                for (skip = 0; skip < 8; skip++) {
                    ni = nf;
                    while (*ni && ((*ni == ' ') || (*ni == '\t'))) ni++;
                    for (nf = ni; *nf && (*nf != ' ') && (*nf != '\t'); nf++);
                    if (!*nf) break;
                    *nf++ = 0;
                }

                // get the sent bytes
                sent_bytes = atoll(ni);
            }

            //
            // generate the result
            strcpy(buffer, "");

            int hasNet = networkAvailable && (prev_nanoseconds >= 0) && 
                        (recv_bytes >= prev_recv_bytes) && (sent_bytes >= prev_sent_bytes);

            if (!networkAvailable) {
                sprintf(cad, "  %s is down", interface);
                strcat(buffer, cad);
            }
            else if (!hasNet) {
                strcat(buffer, "     ? kbps IN \n     ? kbps OUT");
            }
            else {
                long long elapsed = nanoseconds - prev_nanoseconds;
                if (elapsed < 1) elapsed = 1;
                double seconds = elapsed / 1000000000.0;
                long long sent = sent_bytes - prev_sent_bytes;
                long long received = recv_bytes - prev_recv_bytes;
                        // adding 999 ensures "1" for any rate above 0
                long inbps = (long) (8 * received / seconds + 999); 
                long outbps = (long) (8 * sent / seconds + 999);
                if (inbps < 1000000)
                    sprintf(cad, "DOWN: %6ld kbps, ", inbps/1000);
                else
                    sprintf(cad, "DOWN: %6.3f Mbps, ", inbps/1000000.0);
                strcat(buffer, cad);

                if (outbps < 1000000)
                    sprintf(cad, "UP: %6ld kbps", outbps/1000);
                else
                    sprintf(cad, "UP: %6.3f Mbps", outbps/1000000.0);
                strcat(buffer, cad);
                
            }

            /* Save values for next iteration */
            prev_recv_bytes = recv_bytes;
            prev_sent_bytes = sent_bytes;
            prev_nanoseconds = nanoseconds;

            return sprintf(outwalk, buffer);
        #else
            /* This code path requires root privileges */
            int ethspeed = 0;
            struct ifreq ifr;
            struct ethtool_cmd ecmd;

            ecmd.cmd = ETHTOOL_GSET;
            (void)memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_data = (caddr_t)&ecmd;
            (void)strcpy(ifr.ifr_name, interface);
            if (ioctl(general_socket, SIOCETHTOOL, &ifr) == 0) {
                    ethspeed = (ecmd.speed == USHRT_MAX ? 0 : ecmd.speed);
                    return sprintf(outwalk, "%d Mbit/s", ethspeed);
            } else return sprintf(outwalk, "?");
        #endif
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
        char *ethspeed;
        struct ifmediareq ifm;
        (void)memset(&ifm, 0, sizeof(ifm));
        (void)strncpy(ifm.ifm_name, interface, sizeof(ifm.ifm_name));
        int ret = ioctl(general_socket, SIOCGIFMEDIA, (caddr_t)&ifm);

        /* Get the description of the media type, partially taken from
         * FreeBSD's ifconfig */
        const struct ifmedia_description *desc;
        struct ifmedia_description ifm_subtype_descriptions[] =
                IFM_SUBTYPE_ETHERNET_DESCRIPTIONS;

        for (desc = ifm_subtype_descriptions;
             desc->ifmt_string != NULL;
             desc++) {
            if (IFM_TYPE_MATCH(desc->ifmt_word, ifm.ifm_active) &&
                IFM_SUBTYPE(desc->ifmt_word) == IFM_SUBTYPE(ifm.ifm_active))
                break;
        }
        ethspeed = (desc->ifmt_string != NULL ? desc->ifmt_string : "?");
        return sprintf(outwalk, "%s", ethspeed);
#elif defined(__OpenBSD__) || defined(__NetBSD__)
	char *ethspeed;
	struct ifmediareq ifmr;

	(void) memset(&ifmr, 0, sizeof(ifmr));
	(void) strlcpy(ifmr.ifm_name, interface, sizeof(ifmr.ifm_name));

	if (ioctl(general_socket, SIOCGIFMEDIA, (caddr_t)&ifmr) < 0) {
                if (errno != E2BIG)
			return sprintf(outwalk, "?");
	}

	struct ifmedia_description *desc;
	struct ifmedia_description ifm_subtype_descriptions[] =
	    IFM_SUBTYPE_DESCRIPTIONS;

        for (desc = ifm_subtype_descriptions; desc->ifmt_string != NULL; desc++) {
		/*
		 * Skip these non-informative values and go right ahead to the
		 * actual speeds.
		 */
		if (strncmp(desc->ifmt_string, "autoselect", strlen("autoselect")) == 0 ||
		    strncmp(desc->ifmt_string, "auto", strlen("auto")) == 0)
			continue;

		if (IFM_TYPE_MATCH(desc->ifmt_word, ifmr.ifm_active) &&
		    IFM_SUBTYPE(desc->ifmt_word) == IFM_SUBTYPE(ifmr.ifm_active))
			break;
        }
        ethspeed = (desc->ifmt_string != NULL ? desc->ifmt_string : "?");
        return sprintf(outwalk, "%s", ethspeed);

#else
	return sprintf(outwalk, "?");
#endif
}

/*
 * Combines ethernet IP addresses and speed (if requested) for displaying
 *
 */
void print_eth_info(yajl_gen json_gen, char *buffer, const char *interface, const char *format_up, const char *format_down) {
        const char *walk;
        const char *ip_address = get_ip_addr(interface);
        char *outwalk = buffer;

        INSTANCE(interface);

        if (ip_address == NULL) {
                START_COLOR("color_bad");
                outwalk += sprintf(outwalk, "%s", format_down);
                goto out;
        }

        START_COLOR("color_good");

        for (walk = format_up; *walk != '\0'; walk++) {
                if (*walk != '%') {
                        *(outwalk++) = *walk;
                        continue;
                }

                if (strncmp(walk+1, "ip", strlen("ip")) == 0) {
                        outwalk += sprintf(outwalk, "%s", ip_address);
                        walk += strlen("ip");
                } else if (strncmp(walk+1, "speed", strlen("speed")) == 0) {
                        outwalk += print_eth_speed(outwalk, interface);
                        walk += strlen("speed");
                }
        }

out:
        END_COLOR;
        OUTPUT_FULL_TEXT(buffer);
}
