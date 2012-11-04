/******************************************************************************
Author: Samuel Jero <sj323707@ohio.edu>

Date: 10/2012

Description: Program to ping hosts using DCCP REQ packets to test for DCCP connectivity.
******************************************************************************/
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <linux/dccp.h>
#include "checksums.h"


#define MAX(x,y) (x>y ? x : y)
/*Structure for simpler IPv4/IPv6 Address handling*/
typedef union ipaddr{
	struct sockaddr *gen;
	struct sockaddr_in *ipv4;
	struct sockaddr_in6 *ipv6;
} ipaddr_ptr_t;

enum responses{
	UNKNOWN=0,
	RESET,
	RESPONSE,
	SYNC,
	DEST_UNREACHABLE,
	TTL_EXPIRATION,
	TOO_BIG,
	PARAMETER_PROBLEM
};
char* response_label[]= {
"Unknown",
"Closed Port (Reset)",
"Open Port (Response)",
"Open Port (Sync)",
"Destination Unreachable",
"TTL Expiration",
"Packet Too Big",
"DCCP Not Supported (Parameter Problem)"
};


struct request{
	int				seq;
	int				num_replies;
	int				num_errors;
	struct timeval	sent;
	struct timeval	reply;
	enum responses	reply_type;
	struct request  *next;
	struct request  *prev;
};

struct stats{
	int				requests_sent;
	int				replies_received;
	int				errors;
	double  		rtt_min;
	double			rtt_avg;
	double 			rtt_max;
	struct timeval 	start;
	struct timeval 	stop;
};

struct request_queue{
	struct request *head;
	struct request *tail;
};


int debug=0;			/*set to 1 to turn on debugging information*/
int count=-1;			/*Default number of pings (-1 is infinity)*/
int dest_port=33434;	/*Default port*/
int ttl=64;				/*Default TTL*/
long interval=1000;		/*Default delay between pings in ms*/
int ip_type=AF_UNSPEC;	/*IPv4 or IPv6*/
ipaddr_ptr_t dest_addr;	/*Destination Address*/
ipaddr_ptr_t src_addr;	/*Source Address*/
struct request_queue 	queue;
struct stats			ping_stats;
extern int errno;


void getAddresses(char *src, char* dst);
void doping();
void handleDCCPpacket(int rcv_socket, int send_socket);
void handleICMP4packet(int rcv_socket);
void handleICMP6packet(int rcv_socket);
void buildRequestPacket(unsigned char* buffer, int *len, int seq);
void updateRequestPacket(unsigned char* buffer, int *len, int seq);
int logPacket(int seq);
int logResponse(ipaddr_ptr_t *src, int seq, int type);
void clearQueue();
void sigHandler();
void usage();
void sanitize_environment();
void dbgprintf(int level, const char *fmt, ...);


/*Parse commandline options*/
int main(int argc, char *argv[])
{
	char c;
	char *src=NULL;
	char *dst=NULL;
	queue.head=NULL;
	queue.tail=NULL;
	ping_stats.replies_received=0;
	ping_stats.requests_sent=0;
	ping_stats.rtt_avg=0;
	ping_stats.rtt_max=0;
	ping_stats.rtt_min=0;
	ping_stats.errors=0;

	sanitize_environment();

	while ((c = getopt(argc, argv, "64c:p:i:dt:S:")) != -1) {
		switch (c) {
			case '6':
				ip_type=AF_INET6;
				break;
			case '4':
				ip_type=AF_INET;
				break;
			case 'c':
				count = atoi(optarg);
				if(count<=0){
					dbgprintf(0, "Error: count must be positive");
					exit(1);
				}
				break;
			case 'p':
				dest_port = atoi(optarg);
				break;
			case 'i':
				interval = (long)(atof(optarg) * 1000.0);
				if (interval <= 0) {
					fprintf(stderr, "Invalid interval\n");
					exit(1);
				}
				break;
			case 'd':
				debug++;
				break;
			case 't':
				ttl = atoi(optarg);
				if (ttl < 1 || ttl > 255) {
					fprintf(stderr, "Invalid TTL\n");
				}
				break;
			case 'S':
				src=optarg;
				break;
			default:
				usage();
				break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage();
	}
	dst=argv[0];

	getAddresses(src, dst);
	if(src_addr.gen==NULL || dest_addr.gen==NULL){
		dbgprintf(0,"Error: Can't determine source or destination address\n");
		exit(1);
	}

	signal(SIGINT, sigHandler);
	doping();

	free(src_addr.gen);
	free(dest_addr.gen);
	clearQueue();
	return 0;
}

void getAddresses(char *src, char* dst){
	struct addrinfo hint;
	struct addrinfo *dtmp, *stmp;
	struct ifaddrs *temp, *cur;
	struct sockaddr_in6* iv6;
	int addrlen;
	int err;

	/*Lookup destination Address*/
	memset(&hint,0,sizeof(struct addrinfo));
	hint.ai_family=ip_type;
	hint.ai_flags=AI_V4MAPPED | AI_ADDRCONFIG;

	if((err=getaddrinfo(dst,NULL,&hint,&dtmp))!=0){
		dbgprintf(0,"Error: Couldn't lookup destination %s (%s)\n", dst, gai_strerror(err));
		exit(1);
	}
	if(dtmp==NULL){
		dbgprintf(0,"Error: Unknown Host %s\n", dst);
		exit(1);
	}else{
		addrlen=dtmp->ai_addrlen;
		hint.ai_family=ip_type=dtmp->ai_family;
		dest_addr.gen=malloc(dtmp->ai_addrlen);
		if(dest_addr.gen==NULL){
			dbgprintf(0,"Error: Can't allocate Memory\n");
			exit(1);
		}
		memcpy(dest_addr.gen,dtmp->ai_addr,dtmp->ai_addrlen);
	}
	freeaddrinfo(dtmp);

	/*Get a meaningful source address*/
	if(src!=NULL){
		/*Use Commandline arg*/
		if((err=getaddrinfo(src,NULL,&hint,&stmp))!=0){
			dbgprintf(0,"Error: Source Address %s is invalid (%s)\n", src, gai_strerror(err));
			exit(1);
		}
		if(stmp==NULL){
			dbgprintf(0,"Error: Unknown Host %s\n", dst);
			exit(1);
		}else{
			addrlen=stmp->ai_addrlen;
			src_addr.gen=malloc(stmp->ai_addrlen);
			if(src_addr.gen==NULL){
				dbgprintf(0,"Error: Can't allocate Memory\n");
				exit(1);
			}
			memcpy(src_addr.gen,stmp->ai_addr,stmp->ai_addrlen);
		}
		freeaddrinfo(stmp);
	}else{
		/*Guess a good source address*/
		getifaddrs(&temp);
		cur=temp;
		while(cur!=NULL){
			if(cur->ifa_addr==NULL || cur->ifa_addr->sa_family!=ip_type){ /*Not matching ipv4/ipv6 of dest*/
				cur=cur->ifa_next;
				continue;
			}
			if(cur->ifa_flags & IFF_LOOPBACK){ /*Don't use loopback addresses*/
				cur=cur->ifa_next;
				continue;
			}
			if(cur->ifa_addr!=NULL && cur->ifa_addr->sa_family==AF_INET6){
				iv6=(struct sockaddr_in6*)cur->ifa_addr;

				if(iv6->sin6_scope_id!=0){ /*Not globally valid address, if ipv6*/
					cur=cur->ifa_next;
					continue;
				}
			}

			src_addr.gen=malloc(sizeof(struct sockaddr_storage));
			if(src_addr.gen==NULL){
				dbgprintf(0,"Error: Can't allocate Memory\n");
				exit(1);
			}
			src_addr.gen->sa_family=ip_type;
			memcpy(src_addr.gen,cur->ifa_addr,addrlen);
			//break;
			cur=cur->ifa_next;
		}
		freeifaddrs(temp);
	}
	return;
}

/*Preform the ping functionality*/
void doping(){
	int rs, is4,is6,ds;
	int done=0;
	int addrlen;
	int slen=1500;
	unsigned char sbuffer[slen];
	fd_set sel;
	struct timeval timeout;
	struct timeval t,delay, add;
	char pbuf[1000];
	int seq=1;

	/*Open Sockets*/
	rs=socket(ip_type, SOCK_RAW ,IPPROTO_RAW);
	if(rs<0){
		dbgprintf(0, "Error opening raw socket\n");
		exit(1);
	}
	ds=socket(ip_type, SOCK_RAW ,IPPROTO_DCCP);
	if(ds<0){
		dbgprintf(0, "Error opening raw DCCP socket\n");
		exit(1);
	}
	is4=socket(ip_type,SOCK_RAW,IPPROTO_ICMP);
	if(is4<0){
		dbgprintf(0,"Error opening raw ICMPv4 socket\n");
		exit(1);
	}
	is6=socket(ip_type,SOCK_RAW,IPPROTO_ICMPV6);
	if(is6<0){
		dbgprintf(0,"Error opening raw ICMPv6 socket\n");
		exit(1);
	}


	/*Build DCCP packet*/
	buildRequestPacket(sbuffer,&slen,seq);
	if(ip_type==AF_INET){
		addrlen=sizeof(struct sockaddr_in);
	}else{
		addrlen=sizeof(struct sockaddr_in6);
	}

	/*Start Message*/
	if(ip_type==AF_INET){
		dbgprintf(0, "PINGING %s on DCCP port %i\n",
				inet_ntop(ip_type, (void*)&dest_addr.ipv4->sin_addr, pbuf, 1000),dest_port);
	}else{
		dbgprintf(0, "PINGING %s on DCCP port %i\n",
				inet_ntop(ip_type, (void*)&dest_addr.ipv6->sin6_addr, pbuf, 1000),dest_port);
	}

	while(!done){
		/*Send Ping*/
		if(sendto(rs, &sbuffer, slen, MSG_DONTWAIT,(struct sockaddr*)dest_addr.gen,addrlen)<0){
			if(errno!=EINTR){
				dbgprintf(0,"Error: sendto failed\n");
			}
		}
		if(count==0){done=1; break;}

		if (logPacket(seq)<0){
			dbgprintf(0,"Error: Couldn't record request!\n");
		}
		if(ip_type==AF_INET){
			dbgprintf(1, "Sending DCCP Request to %s\n",inet_ntop(ip_type, (void*)&dest_addr.ipv4->sin_addr, pbuf, 1000));
		}else{
			dbgprintf(1, "Sending DCCP Request to %s\n",inet_ntop(ip_type, (void*)&dest_addr.ipv6->sin6_addr, pbuf, 1000));
		}

		/*Use select to wait on packets or until interval has passed*/
		add.tv_sec=interval/1000;
		add.tv_usec=(interval%1000)*1000;
		gettimeofday(&t,NULL);
		timeradd(&t,&add,&delay);
		while(timercmp(&t,&delay,<)){
			/*Prepare for select*/
			FD_ZERO(&sel);
			FD_SET(ds,&sel);
			FD_SET(is4,&sel);
			FD_SET(is6,&sel);
			timersub(&delay,&t,&timeout);

			/*Do select call*/
			if(select(MAX(ds+1,MAX(is4+1,is6+1)),&sel, NULL,NULL,&timeout)<0){
				if(errno!=EINTR){
					dbgprintf(0,"Select() error (%s)\n",strerror(errno));
				}
			}
			if(count==0){done=1;break;}

			if(FD_ISSET(ds,&sel)){
				/*Data on the DCCP socket*/
				handleDCCPpacket(ds,rs);

			}
			if(FD_ISSET(is4,&sel) && ip_type==AF_INET){
				/*Data on the ICMPv4 socket*/
				handleICMP4packet(is4);
			}
			if(FD_ISSET(is6,&sel) && ip_type==AF_INET6){
				/*Data on the ICMPv6 socket*/
				handleICMP6packet(is6);
			}
			gettimeofday(&t,NULL);
		}

		/*Update count*/
		if(count>-1){
			count--;
		}
		seq++;
		updateRequestPacket(sbuffer,&slen, seq);
	}

	close(rs);
	close(is4);
	close(is6);
	close(ds);
}

void handleDCCPpacket(int rcv_socket, int send_socket){
	int rlen=1500;
	unsigned char rbuffer[rlen];
	ipaddr_ptr_t rcv_addr;
	socklen_t rcv_addr_len;
	struct dccp_hdr *dhdr;
	struct dccp_hdr_reset *dhdr_re;
	struct dccp_hdr_response *dhdr_rp;
	struct dccp_hdr_ack_bits *dhdr_sync;
	unsigned char* ptr;
	struct iphdr* iph;

	/*Memory for socket address*/
	rcv_addr_len=sizeof(struct sockaddr_storage);
	rcv_addr.gen=malloc(rcv_addr_len);
	if(rcv_addr.gen==NULL){
		dbgprintf(0,"Error: Can't Allocate Memory!\n");
		exit(1);
	}

	/*Receive Packet*/
	rcv_addr_len=sizeof(struct sockaddr_storage);
	if((rlen=recvfrom(rcv_socket, &rbuffer, 1000,0,rcv_addr.gen,&rcv_addr_len))<0){
		if(errno!=EINTR){
			dbgprintf(0, "Error on receive from DCCP socket (%s)\n",strerror(errno));
		}
	}
	if(rlen<0){
		return;
	}

	if(rcv_addr.gen->sa_family!=ip_type){ //confirm IP type
		dbgprintf(1, "DCCP packet on %s. Tossing.\n", (ip_type==AF_INET) ? "IPv4" : "IPv6");
		free(rcv_addr.gen);
		return;
	}

	if(rcv_addr.gen->sa_family==AF_INET){
		/*IPv4*/
		if(memcmp(&rcv_addr.ipv4->sin_addr,&dest_addr.ipv4->sin_addr,
				sizeof(dest_addr.ipv4->sin_addr))!=0){ //not from destination
			dbgprintf(1,"DCCP packet from 3rd host\n");
			free(rcv_addr.gen);
			return;
		}
		if(rlen < sizeof(struct dccp_hdr)+sizeof(struct iphdr)){ //check packet size

			dbgprintf(1, "Packet smaller than possible DCCP packet received on DCCP socket\n");
			free(rcv_addr.gen);
			return;
		}
		iph=(struct iphdr*)rbuffer;
		ptr=rbuffer+iph->ihl*4;
	}else{
		/*IPv6*/
		if(memcmp(&rcv_addr.ipv6->sin6_addr, &dest_addr.ipv6->sin6_addr,
				sizeof(dest_addr.ipv6->sin6_addr))!=0){ //not from destination
			dbgprintf(1,"DCCP packet from 3rd host\n");
			free(rcv_addr.gen);
			return;
		}
		if(rlen < sizeof(struct dccp_hdr)){ //check packet size

			dbgprintf(1, "Packet smaller than possible DCCP packet received on DCCP socket\n");
			free(rcv_addr.gen);
			return;
		}
		ptr=rbuffer;
	}

	/*DCCP checks*/
	dhdr=(struct dccp_hdr*)ptr;
	if(dhdr->dccph_sport!=htons(dest_port)){
		dbgprintf(1,"DCCP packet with wrong Source Port (%i)\n", ntohs(dhdr->dccph_sport));
		free(rcv_addr.gen);
		return;
	}
	if(dhdr->dccph_dport!=htons(dest_port)){
		dbgprintf(1,"DCCP packet with wrong Destination Port\n");
		free(rcv_addr.gen);
		return;
	}

	/*Pick Response*/
	if(dhdr->dccph_type==DCCP_PKT_RESET){
		if(rlen < (ptr-rbuffer)+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)+sizeof(struct dccp_hdr_reset)){
			dbgprintf(1, "Error: Reset packet too small!");
			return;
		}
		dhdr_re=(struct dccp_hdr_reset*)(ptr+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext));
		logResponse(&rcv_addr, ntohl(dhdr_re->dccph_reset_ack.dccph_ack_nr_low), RESET);
		/*Nothing else to do*/
	}
	if(dhdr->dccph_type==DCCP_PKT_RESPONSE){
		if(rlen < (ptr-rbuffer)+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)+sizeof(struct dccp_hdr_response)){
			dbgprintf(1, "Error: Response packet too small!");
			return;
		}
		dhdr_rp=(struct dccp_hdr_response*)(ptr+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext));
		logResponse(&rcv_addr,ntohl(dhdr_rp->dccph_resp_ack.dccph_ack_nr_low),RESPONSE);
		/*TODO:Send Close back*/
	}
	if(dhdr->dccph_type==DCCP_PKT_SYNC || dhdr->dccph_type==DCCP_PKT_SYNCACK){
		if(rlen < (ptr-rbuffer)+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)+sizeof(struct dccp_hdr_ack_bits)){
			dbgprintf(1, "Error: Response packet too small!");
			return;
		}
		dhdr_sync=(struct dccp_hdr_ack_bits*)(ptr+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext));
		logResponse(&rcv_addr,ntohl(dhdr_sync->dccph_ack_nr_low),SYNC);
		/*TODO:Send Reset*/
	}

	free(rcv_addr.gen);
}

void handleICMP4packet(int rcv_socket){
	int rlen=1500;
	unsigned char rbuffer[rlen];
	ipaddr_ptr_t rcv_addr;
	socklen_t rcv_addr_len;
	struct icmphdr *icmp4;
	struct dccp_hdr *dhdr;
	struct dccp_hdr_ext *dhdre;
	struct iphdr* ip4hdr;
	int type;

	/*Memory for socket address*/
	rcv_addr_len=sizeof(struct sockaddr_storage);
	rcv_addr.gen=malloc(rcv_addr_len);
	if(rcv_addr.gen==NULL){
		dbgprintf(0,"Error: Can't Allocate Memory!\n");
		exit(1);
	}

	/*Receive Packet*/
	if((rlen=recvfrom(rcv_socket, &rbuffer, 1000,0,rcv_addr.gen,&rcv_addr_len))<0){
		if(errno!=EINTR){
			dbgprintf(0, "Error on receive from ICMPv4 socket (%s)\n",strerror(errno));
		}
	}
	if(rlen<0){
		return;
	}

	if(rlen < sizeof(struct icmphdr)){ //check packet size
		dbgprintf(1, "Packet smaller than possible ICMPv4 packet!\n");
		free(rcv_addr.gen);
		return;
	}

	icmp4=(struct icmphdr*)rbuffer;
	if(icmp4->type!=ICMP_DEST_UNREACH && icmp4->type!=ICMP_TIME_EXCEEDED){ //check icmp types
		dbgprintf(1, "Tossing ICMPv4 packet of type %i\n", icmp4->type);
		free(rcv_addr.gen);
		return;
	}

	/*Check packet size again*/
	if(rlen<sizeof(struct icmphdr)+sizeof(struct iphdr)+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)){
		dbgprintf(1, "Tossing ICMPv4 packet that's too small to contain DCCP header!\n");
		free(rcv_addr.gen);
		return;
	}

	/*Decode IPv4 header*/
	ip4hdr=(struct iphdr*)(rbuffer+sizeof(struct icmphdr));
	if(memcmp(&src_addr.ipv4->sin_addr,&ip4hdr->saddr,sizeof(src_addr.ipv4->sin_addr))!=0){
		/*Source address doesn't match*/
		free(rcv_addr.gen);
		return;
	}
	if(memcmp(&dest_addr.ipv4->sin_addr,&ip4hdr->daddr,sizeof(dest_addr.ipv4->sin_addr))!=0){
		/*Destination address doesn't match*/
		free(rcv_addr.gen);
		return;
	}
	if(ip4hdr->protocol!=IPPROTO_DCCP){
		/*Not DCCP!*/
		free(rcv_addr.gen);
		return;
	}

	/*Decode DCCP header*/
	dhdr=(struct dccp_hdr*)(rbuffer+sizeof(struct icmphdr)+ip4hdr->ihl*4);
	if(dhdr->dccph_dport!=htons(dest_port)){
		/*DCCP Destination Ports don't match*/
		free(rcv_addr.gen);
		return;
	}
	if(dhdr->dccph_sport!=htons(dest_port)){
		/*DCCP Source Ports don't match*/
		free(rcv_addr.gen);
		return;
	}
	dhdre=(struct dccp_hdr_ext*)(rbuffer+sizeof(struct icmphdr)+ip4hdr->ihl*4+sizeof(struct dccp_hdr));

	/*Log*/
	if(icmp4->type==ICMP_DEST_UNREACH){
		type=DEST_UNREACHABLE;
	}
	if(icmp4->type==ICMP_TIME_EXCEEDED){
		type=TTL_EXPIRATION;
	}
	logResponse(&rcv_addr,ntohl(dhdre->dccph_seq_low),type);
	free(rcv_addr.gen);
	return;
}

void handleICMP6packet(int rcv_socket){
	int rlen=1500;
	unsigned char rbuffer[rlen];
	ipaddr_ptr_t rcv_addr;
	socklen_t rcv_addr_len;
	struct icmp6_hdr *icmp6;
	struct ip6_hdr* ip6hdr;
	struct dccp_hdr *dhdr;
	struct dccp_hdr_ext *dhdre;
	int type;

	/*Memory for socket address*/
	rcv_addr_len=sizeof(struct sockaddr_storage);
	rcv_addr.gen=malloc(rcv_addr_len);
	if(rcv_addr.gen==NULL){
		dbgprintf(0,"Error: Can't Allocate Memory!\n");
		exit(1);
	}

	/*Receive Packet*/
	if((rlen=recvfrom(rcv_socket, &rbuffer, 1000,0,rcv_addr.gen,&rcv_addr_len))<0){
		dbgprintf(0, "Error on receive from ICMPv6 socket (%s)\n",strerror(errno));
	}

	if(rlen < sizeof(struct icmp6_hdr)){ //check packet size
		dbgprintf(1, "Packet smaller than possible ICMPv6 packet!\n");
		free(rcv_addr.gen);
		return;
	}

	icmp6=(struct icmp6_hdr*)rbuffer;
	if(icmp6->icmp6_type!=ICMP6_DST_UNREACH && icmp6->icmp6_type!=ICMP6_PACKET_TOO_BIG
			&& icmp6->icmp6_type!=ICMP6_TIME_EXCEEDED && icmp6->icmp6_type!=ICMP6_PARAM_PROB){ //check icmp types
		dbgprintf(1, "Tossing ICMPv6 packet of type %i\n", icmp6->icmp6_type);
		free(rcv_addr.gen);
		return;
	}

	/*Check packet size again*/
	if(rlen<sizeof(struct icmp6_hdr)+sizeof(struct ip6_hdr)+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)){
		dbgprintf(1, "Tossing ICMPv6 packet that's too small to contain DCCP header!\n");
		free(rcv_addr.gen);
		return;
	}

	/*Decode IPv6 header*/
	ip6hdr=(struct ip6_hdr*)(rbuffer+sizeof(struct icmp6_hdr));
	if(memcmp(&src_addr.ipv6->sin6_addr,&ip6hdr->ip6_src,sizeof(src_addr.ipv6->sin6_addr))!=0){
		/*Source address doesn't match*/
		free(rcv_addr.gen);
		return;
	}
	if(memcmp(&dest_addr.ipv6->sin6_addr,&ip6hdr->ip6_dst,sizeof(dest_addr.ipv6->sin6_addr))!=0){
		/*Destination address doesn't match*/
		free(rcv_addr.gen);
		return;
	}
	if(ip6hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt!=IPPROTO_DCCP){
		/*Not DCCP!*/
		free(rcv_addr.gen);
		return;
	}

	/*Decode DCCP header*/
	dhdr=(struct dccp_hdr*)(rbuffer+sizeof(struct icmp6_hdr)+sizeof(struct ip6_hdr));
	if(dhdr->dccph_dport!=htons(dest_port)){
		/*DCCP Destination Ports don't match*/
		free(rcv_addr.gen);
		return;
	}
	if(dhdr->dccph_sport!=htons(dest_port)){
		/*DCCP Source Ports don't match*/
		free(rcv_addr.gen);
		return;
	}
	dhdre=(struct dccp_hdr_ext*)(rbuffer+sizeof(struct icmp6_hdr)+sizeof(struct ip6_hdr)+sizeof(struct dccp_hdr));



	/*Log*/
	if(icmp6->icmp6_type==ICMP6_DST_UNREACH){
		type=DEST_UNREACHABLE;
	}
	if(icmp6->icmp6_type==ICMP6_PACKET_TOO_BIG){
		type=TOO_BIG;
	}
	if(icmp6->icmp6_type==ICMP6_TIME_EXCEEDED){
		type=TTL_EXPIRATION;
	}
	if(icmp6->icmp6_type==ICMP6_PARAM_PROB){
		type=PARAMETER_PROBLEM;
	}
	logResponse(&rcv_addr,ntohl(dhdre->dccph_seq_low),type);
	free(rcv_addr.gen);
	return;
}

void buildRequestPacket(unsigned char* buffer, int *len, int seq){
	struct dccp_hdr *dhdr;
	struct dccp_hdr_ext *dhdre;
	struct dccp_hdr_request *dhdrr;
	struct iphdr* ip4hdr;
	struct ip6_hdr* ip6hdr;

	int ip_hdr_len;
	int dccp_hdr_len=sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)+sizeof(struct dccp_hdr_request);

	if(*len < dccp_hdr_len+sizeof(struct ip6_hdr)){
		dbgprintf(0, "Error: Insufficient buffer space\n");
		exit(1);
	}

	memset(buffer, 0, *len);

	/*IP header*/
	ip4hdr=NULL;
	if(ip_type==AF_INET){
		ip_hdr_len=sizeof(struct iphdr);
		ip4hdr=(struct iphdr*)buffer;
		ip4hdr->check=htons(0);
		memcpy(&ip4hdr->daddr, &dest_addr.ipv4->sin_addr, sizeof(dest_addr.ipv4->sin_addr));
		ip4hdr->frag_off=htons(0);
		ip4hdr->id=htons(1);//first
		ip4hdr->ihl=5;
		ip4hdr->protocol=IPPROTO_DCCP;
		memcpy(&ip4hdr->saddr, &src_addr.ipv4->sin_addr, sizeof(src_addr.ipv4->sin_addr));
		ip4hdr->tos=0;
		ip4hdr->tot_len=htons(ip_hdr_len+dccp_hdr_len);
		ip4hdr->ttl=ttl;
		ip4hdr->version=4;
	}else{
		ip_hdr_len=sizeof(struct ip6_hdr);
		ip6hdr=(struct ip6_hdr*)buffer;
		memcpy(&ip6hdr->ip6_dst, &dest_addr.ipv6->sin6_addr, sizeof(dest_addr.ipv6->sin6_addr));
		memcpy(&ip6hdr->ip6_src, &src_addr.ipv6->sin6_addr, sizeof(src_addr.ipv6->sin6_addr));
		ip6hdr->ip6_ctlun.ip6_un1.ip6_un1_flow=htonl(6<<28); //version, traffic class, flow label
		ip6hdr->ip6_ctlun.ip6_un1.ip6_un1_hlim=ttl;
		ip6hdr->ip6_ctlun.ip6_un1.ip6_un1_nxt=IPPROTO_DCCP;
		ip6hdr->ip6_ctlun.ip6_un1.ip6_un1_plen=htons(dccp_hdr_len);
	}

	/*DCCP header*/
	dhdr=(struct dccp_hdr*)(buffer+ip_hdr_len);
	dhdre=(struct dccp_hdr_ext*)(buffer+ip_hdr_len+sizeof(struct dccp_hdr));
	dhdrr=(struct dccp_hdr_request*)(buffer+ip_hdr_len+sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext));
	dhdr->dccph_ccval=0;
	dhdr->dccph_checksum=0;
	dhdr->dccph_cscov=0;
	dhdr->dccph_doff=dccp_hdr_len/4;
	dhdr->dccph_dport=htons(dest_port);
	dhdr->dccph_reserved=0;
	dhdr->dccph_sport=htons(dest_port);
	dhdr->dccph_x=1;
	dhdr->dccph_type=DCCP_PKT_REQUEST;
	dhdr->dccph_seq2=htonl(0); //Reserved if using 48 bit sequence numbers
	dhdr->dccph_seq=htonl(0);  //High 16bits of sequence number. Always make 0 for simplicity.
	dhdre->dccph_seq_low=htonl(seq);
	dhdrr->dccph_req_service= htonl(0x50455246);

	/*Checksums*/
	if(ip_type==AF_INET){
		dhdr->dccph_checksum=ipv4_pseudohdr_chksum((buffer+ip_hdr_len), dccp_hdr_len,
				(unsigned char*) &dest_addr.ipv4->sin_addr,
				(unsigned char*)&src_addr.ipv4->sin_addr, IPPROTO_DCCP);
		ip4hdr->check=ipv4_chksum(buffer,ip_hdr_len);
	}else{
		dhdr->dccph_checksum=ipv6_pseudohdr_chksum((buffer+ip_hdr_len), dccp_hdr_len,
				(unsigned char*) &dest_addr.ipv6->sin6_addr,
				(unsigned char*)&src_addr.ipv6->sin6_addr, IPPROTO_DCCP);
	}
	*len=ip_hdr_len+dccp_hdr_len;
	return;
}

void updateRequestPacket(unsigned char* buffer, int *len, int seq){
	struct dccp_hdr *dhdr;
	struct dccp_hdr_ext *dhdre;
	struct iphdr* ip4hdr;

	int ip_hdr_len;
	int dccp_hdr_len=sizeof(struct dccp_hdr)+sizeof(struct dccp_hdr_ext)+sizeof(struct dccp_hdr_request);

	/*IP header*/
	ip4hdr=NULL;
	if(ip_type==AF_INET){
		ip_hdr_len=sizeof(struct iphdr);
		ip4hdr=(struct iphdr*)buffer;
		ip4hdr->check=htons(0);
		ip4hdr->id=htons(seq);
	}else{
		ip_hdr_len=sizeof(struct ip6_hdr);
	}

	/*DCCP header*/
	dhdr=(struct dccp_hdr*)(buffer+ip_hdr_len);
	dhdre=(struct dccp_hdr_ext*)(buffer+ip_hdr_len+sizeof(struct dccp_hdr));
	dhdr->dccph_checksum=0;
	dhdre->dccph_seq_low=htonl(seq);

	/*Checksums*/
	if(ip_type==AF_INET){
		dhdr->dccph_checksum=ipv4_pseudohdr_chksum((buffer+ip_hdr_len), dccp_hdr_len,
				(unsigned char*) &dest_addr.ipv4->sin_addr,
				(unsigned char*)&src_addr.ipv4->sin_addr, IPPROTO_DCCP);
		ip4hdr->check=ipv4_chksum(buffer,ip_hdr_len);
	}else{
		dhdr->dccph_checksum=ipv6_pseudohdr_chksum((buffer+ip_hdr_len), dccp_hdr_len,
				(unsigned char*) &dest_addr.ipv6->sin6_addr,
				(unsigned char*)&src_addr.ipv6->sin6_addr, IPPROTO_DCCP);
	}
	*len=ip_hdr_len+dccp_hdr_len;
	return;
}

int logPacket(int seq){
	struct request *tmp;

	/*Add new request to queue*/
	tmp=malloc(sizeof(struct request));
	if(tmp==NULL){
		dbgprintf(0,"Error: Can't allocate Memory!\n");
		exit(1);
	}
	tmp->next=NULL;
	tmp->prev=NULL;
	tmp->num_replies=0;
	tmp->num_errors=0;
	tmp->seq=seq;
	tmp->reply_type=UNKNOWN;
	gettimeofday(&tmp->sent,NULL);

	if(queue.head==NULL){
		queue.head=queue.tail=tmp;
	}else{
		queue.head->prev=tmp;
		tmp->next=queue.head;
		queue.head=tmp;
	}

	/*Update Statistics*/
	if(ping_stats.requests_sent==0){
		gettimeofday(&ping_stats.start,NULL);
	}
	ping_stats.requests_sent++;
	return 0;
}

int logResponse(ipaddr_ptr_t *src, int seq, int type){
	struct request *cur;
	double diff;
	char pbuf[1000];

	if(queue.tail==NULL){
		dbgprintf(1,"Response received but no requests sent!\n");
		return -1;
	}

	/*Locate request*/
	cur=queue.tail;
	while(cur!=NULL){
		if(cur->seq==seq){
			gettimeofday(&cur->reply,NULL);
			if(cur->num_replies>0){
				dbgprintf(0,"Duplicate packet detected! (%i)\n", seq);
			}
			if(type<DEST_UNREACHABLE && type!=UNKNOWN){
				cur->num_replies++;
			}else{
				cur->num_errors++;
			}
			cur->reply_type=type;
			break;
		}
		cur=cur->prev;
	}

	if(cur==NULL){
		dbgprintf(1,"Response received but no requests sent with sequence number %i!\n", seq);
		return -1;
	}

	diff=(cur->reply.tv_usec + 1000000*cur->reply.tv_sec) - (cur->sent.tv_usec + 1000000*cur->sent.tv_sec);
	diff=diff/1000.0;

	/*Print Message*/
	if(type<DEST_UNREACHABLE && type!=UNKNOWN){
		if(ip_type==AF_INET){
			dbgprintf(0, "Response from %s : seq=%i  time=%.1fms  status=%s\n",
					inet_ntop(ip_type, (void*)&src->ipv4->sin_addr, pbuf, 1000),
					seq, diff,response_label[type]);
		}else{
			dbgprintf(0, "Response from %s : seq=%i  time=%.1fms  status=%s\n",
					inet_ntop(ip_type, (void*)&src->ipv6->sin6_addr, pbuf, 1000),
					seq, diff,response_label[type]);
		}
	}else{
		if(ip_type==AF_INET){
			dbgprintf(0, "%s from %s : seq=%i\n",response_label[type],
					inet_ntop(ip_type, (void*)&src->ipv4->sin_addr, pbuf, 1000),
					seq);
		}else{
			dbgprintf(0, "%s from %s : seq=%i\n",response_label[type],
					inet_ntop(ip_type, (void*)&src->ipv6->sin6_addr, pbuf, 1000),
					seq);
		}
	}

	/*Update statistics*/
	if(type<DEST_UNREACHABLE && type!=UNKNOWN){
		/*Good Response*/
		if(cur->num_replies==1){
			ping_stats.rtt_avg=((ping_stats.replies_received*ping_stats.rtt_avg)+(diff))/(ping_stats.replies_received+1);
			ping_stats.replies_received++;
		}else{
			ping_stats.errors++;
		}
		if(diff < ping_stats.rtt_min || ping_stats.rtt_min==0){
			ping_stats.rtt_min=diff;
		}
		if(diff > ping_stats.rtt_max){
			ping_stats.rtt_max=diff;
		}
	}else{
		/*Error*/
		ping_stats.errors++;
	}
	gettimeofday(&ping_stats.stop,NULL);
	return 0;
}

void clearQueue(){
	struct request *cur;
	struct request *tmp;

	cur=queue.head;
	while(cur!=NULL){
		tmp=cur;
		cur=cur->next;
		free(tmp);
	}
	queue.head=NULL;
	queue.tail=NULL;
	return;
}

void sigHandler(){
	char pbuf[1000];
	int diff;
	double ploss;

	/*Print Stats*/
	if(ip_type==AF_INET){
		dbgprintf(0,"-----------%s PING STATISTICS-----------\n",
				inet_ntop(ip_type, (void*)&dest_addr.ipv4->sin_addr, pbuf, 1000));
	}else if(ip_type==AF_INET6){
		dbgprintf(0,"-----------%s PING STATISTICS-----------\n",
				inet_ntop(ip_type, (void*)&dest_addr.ipv6->sin6_addr, pbuf, 1000));
	}
	diff=(ping_stats.stop.tv_usec + 1000000*ping_stats.stop.tv_sec) -
			(ping_stats.start.tv_usec + 1000000*ping_stats.start.tv_sec);
	diff=diff/1000.0;
	ploss=(1.0*(ping_stats.requests_sent-ping_stats.replies_received)/ping_stats.requests_sent*1.0)*100;
	dbgprintf(0,"%i packets transmitted, %i received, %i errors, %.2f%% loss, time %ims\n",
			ping_stats.requests_sent,ping_stats.replies_received,ping_stats.errors,
			ploss,diff);
	dbgprintf(0,"rtt min/avg/max = %.1f/%.1f/%.1f ms\n",
			ping_stats.rtt_min,ping_stats.rtt_avg,ping_stats.rtt_max);


	/*Exit Quickly*/
	count=0;
}

/*Usage information for program*/
void usage()
{
	dbgprintf(0, "dccpping: [-d] [-6|-4] [-c count] [-p port] [-i interval] [-t ttl] [-S srcaddress] remote_host\n");
	exit(0);
}

/*Program will probably be run setuid, so be extra careful*/
void sanitize_environment()
{
#if defined(_SVID_SOURCE) || defined(_XOPEN_SOURCE)
	clearenv();
#else
	extern char **environ;
	environ = NULL;
#endif
}

/*Debug Printf*/
void dbgprintf(int level, const char *fmt, ...)
{
    va_list args;
    if(debug>=level){
    	va_start(args, fmt);
    	vfprintf(stderr, fmt, args);
    	va_end(args);
    }
}
