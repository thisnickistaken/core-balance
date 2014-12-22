/*  Copyright 2010-2011 Steven Johnson */
/*  This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include "includes/bitfield.c"
#include "includes/parse.c"
#include "includes/debug.c"

#ifndef _NO_THREADS
#include <pthread.h>
#endif

#define BLOCK_SZ	8192

#define BAL_TEST	0x00000001
#define BAL_KILL	0x00000002
#define BAL_STDIN	0x00000004
#define BAL_WATCH	0x00000008

#define BN_DOWN	0x80000000

typedef void *(*pthread_func)(void*);

typedef struct _BALANCE_NODE
{
	struct sockaddr_in addr;
	unsigned short max_load;
	unsigned short cur_load;
	unsigned long count;
	uint32_t flags;
	struct _BALANCE_NODE *next;
} BALANCE_NODE;

typedef struct _CONNECTION_NODE
{
	struct sockaddr_in addr;
	int s[2];
	unsigned long flags;
	struct _BALANCE_NODE *bal;
	struct _CONNECTION_NODE *next;
} CONNECTION_NODE;

typedef struct _THREAD_NODE
{
#ifndef _NO_THREADS
	pthread_t tid;
#endif
	unsigned long a, w, c, s;
	struct _THREAD_NODE *next;
} THREAD_NODE;

int balance_connections(THREAD_NODE *t);
BALANCE_NODE *ParseNodes(char *file);
int FreeConnectionList(CONNECTION_NODE *list);
int FreeBalanceList(BALANCE_NODE *list);
int FreeThreadList(THREAD_NODE *list);
int RemoveConnection(CONNECTION_NODE *con);
int PrintConnectionList(CONNECTION_NODE *list);
int PrintBalanceList(BALANCE_NODE *list);
int PrintThreadList(THREAD_NODE *list);
int StartWatch();

unsigned long block_sz = BLOCK_SZ, backlog = 0, flags = 0, threads = 2;
int a, w;
struct sockaddr_in l, http;
BALANCE_NODE *blist = NULL;
CONNECTION_NODE *clist = NULL;
THREAD_NODE *tlist = NULL;
#ifndef _NO_THREADS
pthread_mutex_t m_bal, m_con, m_thr;
#endif

int main(int argc, char **argv)
{
	int x;
	char *config = NULL;
	BALANCE_NODE *b = NULL;
	THREAD_NODE *t = NULL;
	
	if(argc == 1)
	{
		printf("Steve's Load Balancing Proxy -- built with distcc in mind :)\n\n"
			"\tUsage: %s [options]\n\n"
			"\t-b <block_size>\t\t--\tUse buffers of <block_size> bytes (default: %u).\n"
			"\t-c <config_file>\t--\tRead configuration and settings from <config_file>.\n"
			"\t-i\t\t\t--\tInteractive mode.\n"
			"\t-p <port_number>\t--\tUse <port_number> as external balanced port (default: first node's port).\n"
#ifndef _NO_THREADS
			"\t-t <number_threads>\t--\tUse <number_threads> to handle connections (default: 2).\n"
#endif
			"\t-v <verbosity_level>\t--\tSet program verbosity to <verbosity_level>.\n"
			"\t-w <port_number>\t--\tAllow \'watching\' via HTTP on <port_number>.\n"
			"\t-x\t\t\t--\tTest configuration.\n", argv[0], BLOCK_SZ);
			return 0;
	}
	l.sin_family = AF_INET;
	l.sin_port ^= l.sin_port;
	l.sin_addr.s_addr = INADDR_ANY;
	http.sin_family = AF_INET;
	http.sin_port ^= http.sin_port;
	http.sin_addr.s_addr = INADDR_ANY;
	for(x = 1; x < argc; x++)
	{
		switch(*argv[x])
		{
			case '-':
				switch(*(argv[x] + 1))
				{
					case 'B':
					case 'b':
						if(++x < argc)
						{
							if(!(block_sz = atoi(argv[x])))
							{
								block_sz = BLOCK_SZ;
								printf("Block size must be a valid, non-zero integer.\n");
							}
						}
						else
							printf("Block size not provided.\n");
						break;
					case 'C':
					case 'c':
						if(++x < argc)
							config = argv[x];
						else
							printf("No argument given for config file.\n");
						break;
					case 'I':
					case 'i':
						set(&flags, BAL_STDIN);
						break;
					case 'P':
					case 'p':
						if(++x < argc)
						{
							if(!(l.sin_port = ntohs(atoi(argv[x]))))
								printf("Port number must be a valid, non-zero integer.\n");
						}
						else
							printf("Port number not provided.\n");
						break;
#ifndef _NO_THREADS
					case 'T':
					case 't':
						if(++x < argc)
						{
							if(!(threads = atoi(argv[x])))
							{
								threads = 2;
								printf("NUmber of threads must be a valid, non-zero integer.\n");
							}
						}
						else
							printf("Number of threads not provided.\n");
						break;
#endif
					case 'V':
					case 'v':
						if(++x < argc)
							bdbgl(atoi(argv[x]));
						else
							printf("Verbosity level not provided.\n");
						break;
					case 'W':
					case 'w':
						if(++x < argc)
						{
							if(!(http.sin_port = htons(atoi(argv[x]))))
								printf("Watch port must be a valid, non-zero integer.\n");
						}
						else
							printf("You must specify a port on which to allow watching.\n");
						break;
					case 'X':
					case 'x':
						set(&flags, BAL_TEST);
						break;
					default:
						printf("Invalid switch \"%s\".\n", argv[x]);
						break;
				}
				break;
			default:
				printf("Unexpected argument \"%s\".\n", argv[x]);
				break;
		}
	}
	bdbg(3) printf("Parsing \"%s\"...\n", config);
	if(!(blist = ParseNodes(config)))
	{
		bdbg(1) printf("Failed to parse config file \"%s\".\n", config);
		return 1;
	}
	bdbg(3) printf("Config file parsed.\n");
	if(!l.sin_port)
		l.sin_port = blist->addr.sin_port;
	if(isset(flags, BAL_TEST))
	{
		PrintBalanceList(blist);
		printf("\n");
		if(FreeBalanceList(blist) != 0)
			bdbg(3) printf("Failed to free balance node list.\n");
		return 0;
	}
	for(b = blist; b != NULL; b = b->next)
		backlog += b->max_load;
	if((a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
	{
		bdbg(1) printf("Failed to create socket.\n");
		if(FreeBalanceList(blist) != 0)
			bdbg(3) printf("Failed to free balance node list.\n");
		return 0;
	}
	x = 1;
	if(setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x)) == -1)
	{
		printf("Failed to set socket options\n");
		return 0;
	}
	if(bind(a, (struct sockaddr*)&l, sizeof(struct sockaddr_in)) == -1)
	{
		bdbg(1) printf("Failed to bind socket to port %d.\n", ntohs(l.sin_port));
		if(FreeBalanceList(blist) != 0)
			bdbg(3) printf("Failed to free balance node list.\n");
		return 0;
	}
	if(listen(a, backlog) == -1)
	{
		bdbg(1) printf("Failed to listen to port %d.\n", ntohs(l.sin_port));
		if(FreeBalanceList(blist) != 0)
			bdbg(3) printf("Failed to free balance node list.\n");
		return 0;
	}
	tlist = malloc(sizeof(THREAD_NODE));
	tlist->next = NULL;
	tlist->a ^= tlist->a;
	tlist->c ^= tlist->c;
	tlist->w ^= tlist->w;
	tlist->s ^= tlist->s;
#ifndef _NO_THREADS
	bdbg(3) printf("Starting threads...\n");
	pthread_mutex_init(&m_bal, NULL);
	pthread_mutex_init(&m_con, NULL);
	pthread_mutex_init(&m_thr, NULL);
	tlist->tid = 0;
	t = tlist;
	for(x ^= x; x < threads - 1; x++)
	{
		t->next = malloc(sizeof(THREAD_NODE));
		t = t->next;
		t->next = NULL;
		t->a ^= t->a;
		t->w ^= t->w;
		t->c ^= t->c;
		t->s ^= t->s;
		if(pthread_create(&t->tid, NULL, (pthread_func)balance_connections, t) == -1)
		{
			bdbg(1) printf("Failed to create thread %d.\n", x + 1);
			if(FreeBalanceList(blist) != 0)
				bdbg(3) printf("Failed to free balance node list.\n");
			return 1;
		}
	}
	bdbg(3) printf("%d threads started.\n", x);
#endif
	if(http.sin_port != 0)
	{
		bdbg(3) printf("Starting watching server.\n");
		StartWatch();
	}
#ifndef _NO_THREADS
	bdbg(3) printf("Main thread entering balance mode.\n");
#else
	bdbg(3) printf("Entering balance mode.\n");
#endif
	balance_connections(tlist);
	
	if(FreeConnectionList(clist) != 0)
		bdbg(3) printf("Failed to free connection list.\n");
	if(FreeThreadList(tlist) != 0)
		bdbg(3) printf("Failed to free thread list.\n");
	if(FreeBalanceList(blist) != 0)
		bdbg(3) printf("Failed to free balance node list.\n");
	
	return 0;
}

int balance_connections(THREAD_NODE *th)
{
	struct timeval tv;;
	fd_set sset;
	int count, tsock;
	struct sockaddr_in taddr;
	int x;
	CONNECTION_NODE *c = NULL, *c2 = NULL;
	BALANCE_NODE *b = NULL;
	THREAD_NODE *t = NULL;
	unsigned char *buf = NULL;
	
	buf = malloc(block_sz);
	while(!isset(flags, BAL_KILL))
	{
		bdbg(7) printf("balance_connections(): Top of loop\n");
		FD_ZERO(&sset);
		if(isset(flags, BAL_STDIN))
		{
			bdbg(7) printf("balance_connections(): BAL_STDIN\n");
			FD_SET(0, &sset);
		}
		if(isset(flags, BAL_WATCH))
		{
			bdbg(7) printf("balance_connections(): BAL_WATCH\n");
			FD_SET(w, &sset);
		}
		FD_SET(a, &sset);
		if(a > w)
			count = a;
		else
			count = w;
#ifndef _NO_THREADS
		pthread_mutex_lock(&m_con);
#endif
		for(c = clist; c != NULL; c = c->next)
		{
			for(x ^= x; x < 2; x++)
			{
				FD_SET(c->s[x], &sset);
				if(c->s[x] > count)
					count = c->s[x];
			}
		}
#ifndef _NO_THREADS
		pthread_mutex_unlock(&m_con);
#endif
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		bdbg(7) printf("balance_connections(): high socket: %u\n", count);
		bdbg(7) printf("balance_connections(): select()...\n");
		count = select(count + 1, &sset, NULL, NULL, &tv);
		switch(count)
		{
			case -1:
				bdbg(1) printf("balance_connections(): select() failed\n");
				break;
			case 0:
				bdbg(7) printf("balance_connections(): no sockets returned from select()\n");
				break;
			default:
				bdbg(7) printf("balance_connections(): processing returned sockets...\n");
				bdbg(7) printf("balance_connections(): checking for waiting proxy connections...\n");
				if(FD_ISSET(a, &sset))
				{
					th->a++;
					bdbg(7) printf("balance_connections(): processing listening socket\n");
#ifndef _NO_THREADS
					pthread_mutex_lock(&m_con);
#endif
					if(clist)
					{
						for(c = clist; c->next != NULL; c = c->next);
						c->next = malloc(sizeof(CONNECTION_NODE));
						c = c->next;
					}
					else
					{
						clist = malloc(sizeof(CONNECTION_NODE));
						c = clist;
					}
					c->flags ^= c->flags;
					*(unsigned long*)&c->next ^= *(unsigned long*)&c->next;
					x = sizeof(struct sockaddr_in);
					bdbg(7) printf("balance_connections(): searching for open node...\n");
#ifndef _NO_THREADS
					pthread_mutex_lock(&m_bal);
#endif
					for(b = blist; b != NULL; b = b->next)
						if(b->max_load > b->cur_load && !isset(b->flags, BN_DOWN))
							break;
					if(!b)
					{
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_bal);
#endif
						bdbg(7) printf("balance_connections(): no open nodes, leaving connection in queue\n");
						RemoveConnection(c);
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_con);
#endif
						c = NULL;
						break;
					}
					b->cur_load++;
					c->bal = b;
#ifndef _NO_THREADS
					pthread_mutex_unlock(&m_bal);
#endif
					if((c->s[1] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
					{
						bdbg(1) printf("balance_connections(): Failed to create socket for proxied connection.\n");
						b->cur_load--;
/*#ifndef _NO_THREADS
						pthread_mutex_lock(&m_con);
#endif*/
						RemoveConnection(c);
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_con);
#endif
						c = NULL;
						break;
					}
#ifndef _NO_THREADS
					pthread_mutex_unlock(&m_con);
#endif
					bdbg(7) printf("balance_connections(): connecting to node...\n");
					if((connect(c->s[1], (struct sockaddr*)&b->addr, sizeof(struct sockaddr_in))) == -1)
					{
						bdbg(1) printf("balance_connections(): Failed to connect to server.\n");
						set((unsigned long*)&b->flags, BN_DOWN);
#ifndef _NO_THREADS
						pthread_mutex_lock(&m_bal);
#endif
						close(c->s[1]);
						b->cur_load--;
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_bal);
						pthread_mutex_lock(&m_con);
#endif
						RemoveConnection(c);
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_con);
#endif
						c = NULL;
						break;
					}
					bdbg(7) printf("balance_connections(): accepting connection...\n");
#ifndef _NO_THREADS
					pthread_mutex_lock(&m_con);
#endif
					if((c->s[0] = accept(a, (struct sockaddr*)&c->addr, (socklen_t*)&x)) == -1)
					{
						bdbg(1) printf("balance_connections(): Failed to accept connection.\n");
						close(c->s[1]);
#ifndef _NO_THREADS
						pthread_mutex_lock(&m_bal);
#endif
						close(c->s[1]);
						b->cur_load--;
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_bal);
#endif
						RemoveConnection(c);
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_con);
#endif
						c = NULL;
						break;
					}
#ifndef _NO_THREADS
					pthread_mutex_unlock(&m_con);
#endif
					b->count++;
				}
				bdbg(7) printf("balance_connections(): Checking for data on connections.\n");
#ifndef _NO_THREADS
				pthread_mutex_lock(&m_con);
#endif
				for(c = clist; c != NULL; c = c->next)
				{
					if(c2 == clist)
					{
						c = clist;
						c2 = NULL;
					}
					bdbg(7) printf("balance_connections(): searching through connections for returned socket\n");
					for(x ^= x; x < 2; x++)
						if(FD_ISSET(c->s[x], &sset))
						{
							th->c++;
							bdbg(7) printf("balance_connections(): processing connection socket c->s[%u] = %u.\n", x, c->s[x] );
							count = read(c->s[x], buf, block_sz);
							bdbg(7) printf("balance_connections(): read data: %d\n", count);
							switch(count)
							{
								case -1:
									bdbg(1) printf("Failed to read from socket.\n");
									count = 1;
									break;
								case 0:
									bdbg(6) printf("Socket closed.\n");
									count = 1;
									break;
								default:
									bdbg(7) printf("balance_connections(): writing %u bytes of data to c->s[%u] = %u\n", count, (x + 1) % 2, c->s[(x + 1) % 2]);
									count = write(c->s[(x + 1) % 2], buf, count);
									bdbg(7) printf("balance_connections(): wrote data: %d\n", count);
									switch(count)
									{
										case -1:
											bdbg(1) printf("Failed to send data.\n");
											close(c->s[x]);
											count = 1;
											break;
										case 0:
											bdbg(1) printf("Socket closed.\n");
											count = 1;
											break;
										default:
											count = 0;
											break;
									}
									break;
							}
							if(count)
							{
								bdbg(6) printf("balance_connections(): cleaning up connections...\n");
								c->bal->cur_load--;
								if(clist == c)
									c2 = clist;
								else
									for(c2 = clist; c2->next != c; c2 = c2->next);
								close(c->s[0]);
								close(c->s[1]);
								//close(c->s[(x + 1) % 2]);
								RemoveConnection(c);
								c = c2;
								if(!x)
									break;
							}
						}
					if(!c)
						break;
				}
#ifndef _NO_THREADS
				pthread_mutex_unlock(&m_con);
#endif
				bdbg(7) printf("balance_connections(): Checking for data on stdin.\n");
				if(FD_ISSET(0, &sset))
				{
					th->s++;
					count = read(0, buf, 1);
					switch(count)
					{
						case -1: 
							bdbg(1) printf("balance_connections(): Failed to read from stdin.\n");
							count = 1;
							break;
						case 0:
							bdbg(1) printf("balance_connections(): Stdin closed.\n");
							count = 1;
							break;
						default:
							switch(*buf)
							{
								case 'B':
								case 'b':
#ifndef _NO_THREADS
									pthread_mutex_lock(&m_bal);
#endif
									if(PrintBalanceList(blist))
										printf("Balance list is empty.\n");
#ifndef _NO_THREADS
									pthread_mutex_unlock(&m_bal);
#endif
									break;
								case 'C':
								case 'c':
#ifndef _NO_THREADS
									pthread_mutex_lock(&m_con);
#endif
									if(PrintConnectionList(clist))
										printf("No connections are open.\n");
#ifndef _NO_THREADS
									pthread_mutex_unlock(&m_con);
#endif
									break;
								case 'Q':
								case 'q':
									printf("Killing threads.\n");
									set(&flags, BAL_KILL);
									break;
								case 'T':
								case 't':
#ifndef _NO_THREADS
									pthread_mutex_lock(&m_thr);
#endif
									if(PrintThreadList(tlist))
										printf("No threads in use.\n");
#ifndef _NO_THREADS
									pthread_mutex_unlock(&m_thr);
#endif
									break;
								case 'V':
								case 'v':
									count = read(0, buf + 1, 1);
									switch(count)
									{
										case -1:
											bdbg(1) printf("balance_connections(): Failed to read verbosity level from stdin.\n");
											count = 1;
											break;
										case 0:
											bdbg(1) printf("balance_connections(): Stdin was closed before vebosity level was read.\n");
											count = 1;
											break;
										default:
											if(buf[1] >= '0' && buf[1] <= '9')
												bdbgl(buf[1] - '0');
											else
												printf("Invalid verbosity level specified: \'%c\'\n", buf[1]);
											count = 0;
											break;
									}
									if(count)
										clear(&flags, BAL_STDIN);
									break;
							}
							count = 0;
					}
					if(count)
						clear(&flags, BAL_STDIN);
				}
				bdbg(7) printf("balance_connections(): Checking for waiting watching connections.\n");
				if(isset(flags, BAL_WATCH))
					if(FD_ISSET(w, &sset))
					{
						th->w++;
						bdbg(7) printf("balance_connections(): Processing watching port.\n");
						x = sizeof(struct sockaddr_in);
						bdbg(7) printf("balance_connections(): Accepting connection.\n");
						if((tsock = accept(w, (struct sockaddr*)&taddr, (socklen_t*)&x)) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to accept watching port connection.\n");
							break;
						}
						bdbg(7) printf("balance_connections(): Writing first header.\n");
						if((count = snprintf(buf, block_sz,
							"HTTP/1.0 200 OK\n"
							"Content-Type: text/html\n\n"
							"<http>\n"
							"<head>\n"
							"\t<title>Steve\'s Load Balancing Proxy</title>\n"
							"</head>\n"
							"<body>\n"
							"<font size=\"3\">\n"
							"<table cellpadding=\"2\" cellspacing=\"0\">\n"
							"\t<tr>\n"
							"\t\t<td>Balanced Port:</td>\n"
							"\t\t<td>%u</td>\n"
							"\t</tr>\n"
							"\t<tr>\n"
							"\t\t<td>Watching Port:</td><td>"
							, ntohs(l.sin_port))) >= block_sz)
						{
							bdbg(3) printf("balance_connections(): snprintf() ran out of buffer\n");
							break;
						}
						if(write(tsock, buf, count) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to write header to watching socket.\n");
							break;
						}
						if(isset((unsigned long)&flags, BAL_WATCH))
						{
							if((count = snprintf(buf, block_sz, "%u", ntohs(http.sin_port))) >= block_sz)
							{
								bdbg(3) printf("balance_connections(): snprintf() ran out of buffer\n");
								break;
							}
							if(write(tsock, buf, count) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write watching status.\n");
								break;
							}
						}
						else
						{
							if(write(tsock, "Disabled", 8) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write watching status.\n");
								break;
							}
						}
						if(write(tsock,
							"</td>\n"
							"\t</tr>\n"
							"\t<tr>\n"
							"\t\t<td>Interactive Mode:</td>\n"
							"\t\t<td>", 54) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to write interactive mode info.\n");
							break;
						}
						if(isset(flags, BAL_STDIN))
						{
							if(write(tsock, "Enabled", 7) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write interactive mode status.\n");
								break;
							}
						}
						else
						{
							if(write(tsock, "Disabled", 8) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write interactive mode status.\n");
								break;
							}
						}
						if(write(tsock,
							"</td>\n"
							"\t</tr>\n"
							"</table>\n"
							"</font>\n"
							"<h3>Balance Nodes</h3>\n"
							"<table cellpadding=\"5\" cellspacing=\"0\">\n"
							"\t<tr>\n"
							"\t\t<td>IP</td>\n"
							"\t\t<td>Port</td>\n"
							"\t\t<td>Max Load</td>\n"
							"\t\t<td>Current Load</td>\n"
							"\t\t<td>Total</td>\n"
							"\t\t<td>Flags</td>\n"
							"\t</tr>\n", 214) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to write second header to watching socket.\n");
							break;
						}
						bdbg(7) printf("balance_connections(): Writing balance node info.\n");
#ifndef _NO_THREADS
						pthread_mutex_lock(&m_bal);
#endif
						for(b = blist; b != NULL; b = b->next)
						{
							if((count = snprintf(buf, block_sz,
								"\t<tr>\n"
								"\t\t<td>%u.%u.%u.%u</td>\n"
								"\t\t<td>%u</td>\n"
								"\t\t<td>%u</td>\n"
								"\t\t<td>%u</td>\n"
								"\t\t<td>%lu</td>\n"
								"\t\t<td>0x%08x</td>\n"
								"\t</tr>\n", *(unsigned char*)&b->addr.sin_addr.s_addr, *(((unsigned char*)&b->addr.sin_addr.s_addr) + 1), *(((unsigned char*)&b->addr.sin_addr.s_addr) + 2), *(((unsigned char*)&b->addr.sin_addr.s_addr) + 3), ntohs(b->addr.sin_port), b->max_load, b->cur_load, b->count, b->flags)) >=  block_sz)
							{
								bdbg(3) printf("balance_connections(): snprintf() ran out of buffer\n");
								break;
							}
							if(write(tsock, buf, count) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write balance node row to watching socket.\n");
								break;
							}
						}
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_bal);
#endif
						bdbg(7) printf("balance_connections(): Writing third header.\n");
						if(write(tsock,
							"</table>\n"
							"<br />\n"
#ifndef _NO_THREADS
							"<h3>Thread Nodes</h3>\n"
#else
							"<h3>Work Counter</h3>\n"
#endif
							"<table cellpadding=\"5\" cellspacing=\"0\">\n"
							"\t<tr>\n"
#ifndef _NO_THREADS
							"\t\t<td>Thread ID</td>\n"
#endif
							"\t\t<td>Accept</td>\n"
							"\t\t<td>Connection</td>\n"
							"\t\t<td>Watch</td>\n"
							"\t\t<td>Stdin</td>\n"
#ifndef _NO_THREADS
							"\t</tr>\n",186) == -1)
#else
							"\t</tr>\n",165) == -1)
#endif
						{
							bdbg(3) printf("balance_connections(): Failed to write third header to watching socket.\n");
							break;
						}
						bdbg(7) printf("balance_connections(): Writing thread list.\n");
#ifndef _NO_THREADS
						pthread_mutex_lock(&m_thr);
#endif
						for(t = tlist; t != NULL; t = t->next)
						{
							if((count = snprintf(buf, block_sz,
								"\t<tr>\n"
#ifndef _NO_THREADS
								"\t\t<td>%u</td>\n"
#endif
								"\t\t<td>%lu</td>\n"
								"\t\t<td>%lu</td>\n"
								"\t\t<td>%lu</td>\n"
								"\t\t<td>%lu</td>\n"
								"\t</tr>\n",
#ifndef _NO_THREADS
								(unsigned int)t->tid,
#endif
								t->a,
								t->c,
								t->w,
								t->s)) >=  block_sz)
							{
								bdbg(3) printf("balance_connections(): snprintf() ran out of buffer\n");
								break;
							}
							if(write(tsock, buf, count) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write thread node row to watching socket.\n");
								break;
							}
						}
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_thr);
#endif
						bdbg(7) printf("balance_connections(): Writing fourth header.\n");
						if(write(tsock,
							"</table>\n"
							"<br />\n"
							"<h3>Connection Nodes</h3>\n"
							"<table cellpadding=\"5\" cellspacing=\"0\">\n"
							"\t<tr>\n"
							"\t\t<td>IP</td>\n"
							"\t\t<td>Port</td>\n"
							"\t\t<td>Node</td>\n"
							"\t\t<td>Port</td>\n"
							"\t\t<td>Flags</td>\n"
							"\t</tr>\n",174) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to write second header to watching socket.\n");
							break;
						}
						bdbg(7) printf("balance_connections(): Writing connection list.\n");
#ifndef _NO_THREADS
						pthread_mutex_lock(&m_con);
#endif
						for(c = clist; c != NULL; c = c->next)
						{
							if((count = snprintf(buf, block_sz,
								"\t<tr>\n" 
								"\t\t<td>%u.%u.%u.%u</td>\n"
								"\t\t<td>%u</td>\n"
								"\t\t<td>%u.%u.%u.%u</td>\n"
								"\t\t<td>%u</td>\n"
								"\t\t<td>0x%08x</td>\n"
								"\t</tr>\n",
								*(unsigned char*)&c->addr.sin_addr.s_addr,
								*(((unsigned char*)&c->addr.sin_addr.s_addr) + 1),
								*(((unsigned char*)&c->addr.sin_addr.s_addr) + 2),
								*(((unsigned char*)&c->addr.sin_addr.s_addr) + 3),
								ntohs(c->addr.sin_port),
								*(unsigned char*)&c->bal->addr.sin_addr.s_addr,
								*(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 1),
								*(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 2),
								*(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 3),
								ntohs(c->bal->addr.sin_port), (unsigned int)c->flags)) >=  block_sz)
							{
								bdbg(3) printf("balance_connections(): snprintf() ran out of buffer\n");
								break;
							}
							if(write(tsock, buf, count) == -1)
							{
								bdbg(3) printf("balance_connections(): Failed to write connection node row to watching socket.\n");
								break;
							}
						}
#ifndef _NO_THREADS
						pthread_mutex_unlock(&m_con);
#endif
						bdbg(7) printf("balance_connections(): Writing footer.\n");
						if(write(tsock,
							"</table>\n"
							"</body>\n"
							"</html>\n",25) == -1)
						{
							bdbg(3) printf("balance_connections(): Failed to write footer to watching socket.\n");
							break;
						}
						bdbg(6) printf("balance_connections(): Closing watching socket.\n");
						close(tsock);
					}
				bdbg(7) printf("balance_connections(): End of checks.\n");
				break;
		}
	}
	free(buf);
	buf = NULL;
	
	return 0;
}

BALANCE_NODE *ParseNodes(char *file)
{
	int fd;
	int x, y, cfgc, valc;
	char **cfg = NULL, **val = NULL;
	BALANCE_NODE *bal = NULL, *b = NULL, *blast = NULL;
	struct hostent *h = NULL;
	char *buf = NULL;
	unsigned char quote = 0;
	struct stat st;
	
	if(file == NULL)
		return NULL;
	
	bdbg(5) printf("ParseNodes(): opening file...\n");
	if((fd = open(file, O_RDONLY, 0)) == -1)
	{
		bdbg(3) printf("ParseNodes(): Failed to open \"%s\" for reading.\n", file);
		return NULL;
	}
	if((fstat(fd, &st)) == -1)
	{
		bdbg(3) printf("ParseNodes(): Failed to stat \"%s\".\n", file);
		close(fd);
		return NULL;
	}
	bdbg(5) printf("ParseNodes(): reading file...\n");
	buf = (char*)malloc(st.st_size + 1);
	if((x = read(fd, buf, st.st_size)) == -1)
	{
		bdbg(3) printf("ParseNodes(): Failed to read \"%s\" into memory.\n", file);
		close(fd);
		free(buf);
		buf = NULL;
		return NULL;
	}
	bdbg(5) printf("ParseNodes(): splitting file by lines...\n");
	buf[x] ^= buf[x];
	cfgc = SplitByLine(buf, &cfg);
	free(buf);
	buf = NULL;
	if(cfgc == 0)
	{
		bdbg(3) printf("ParseNodes(): Failed to parse \"%s\" by lines.\n", file);
		close(fd);
		return NULL;
	}
	bdbg(5) printf("ParseNodes(): parsing lines...\n");
	for(x ^= x; x < cfgc; x++)
	{
		switch(*cfg[x])
		{
			case '#':
				bdbg(5) printf("ParseNodes(): found comment at line %u\n", x);
				break;
			default:
				bdbg(5) printf("ParseNodes(): counting arguments...\n");
				for(y ^= y; *(cfg[x] + y) != 0; y++)
				{
					if(*(cfg[x] + y) == '\"' || *(cfg[x] + y) == '\'')
					{
						if(quote == *(cfg[x] + y))
							quote = 0;
						else
							quote = *(cfg[x] + y);
					}
					if(*(cfg[x] + y) == '#' && quote == 0)
						*(cfg[x] + y) = 0;
				}
				if((valc = QSplitByChar(cfg[x], ',', &val)) != 4)
				{
					bdbg(3) printf("ParseNodes(): Line %u has an incorrect number of values. Skipping...\n", x);
					break;
				}
				bdbg(5) printf("ParseNodes(): adding node...\n");
				if(bal == NULL)
				{
					bal = (BALANCE_NODE*)malloc(sizeof(BALANCE_NODE));
					b = bal;
					blast = NULL;
				}
				else
				{
					b->next = (BALANCE_NODE*)malloc(sizeof(BALANCE_NODE));
					blast = b;
					b = b->next;
				}
				b->addr.sin_family = AF_INET;
				bdbg(5) printf("ParseNodes(): resolving hostname...\n");
				if(!(h = gethostbyname(val[0])))
				{
					bdbg(3) printf("ParseNodes(): Failed to resolve \"%s\" in line %u. Skipping...\n", val[0], x);
					freeList(&val, valc);
					val = NULL;
					free(b);
					b = NULL;
					if(!blast)
						bal = NULL;
					else
						blast->next = NULL;
					break;
				}
				b->addr.sin_addr.s_addr = *(unsigned long*)h->h_addr_list[0];
				h = NULL;
				bdbg(5) printf("ParseNodes(): parsing port number...\n");
				if(!(b->addr.sin_port = htons(atoi(val[1]))))
				{
					bdbg(3) printf("ParseNodes(): Invalid port number \"%s\" in line %u.\n", val[1], x);
					freeList(&val, valc);
					val = NULL;
					free(b);
					b = NULL;
					if(!blast)
						bal = NULL;
					else
						blast->next = NULL;
					break;
				}
				bdbg(5) printf("ParseNodes(): parsing maximum load...\n");
				if(!(b->max_load = atoi(val[2])))
				{
					bdbg(3) printf("ParseNodes(): Invalid maximum connection number \"%s\" in line %u.\n", val[2], x);
					freeList(&val, valc);
					val = NULL;
					free(b);
					b = NULL;
					if(!blast)
						bal = NULL;
					else
						blast->next = NULL;
					break;
				}
				b->cur_load ^= b->cur_load;
				b->flags = atoi(val[3]);
				b->count ^= b->count;
				b->next = NULL;
				freeList(&val, valc);
				val = NULL;
				break;
		}
	}
	bdbg(5) printf("ParseNodes(): finished parsing lines\n");
	freeList(&cfg, cfgc);
	cfg = NULL;
	return bal;
}

int FreeConnectionList(CONNECTION_NODE *list)
{
	CONNECTION_NODE * c = NULL, *next = NULL;
	
	if(list == NULL)
		return 1;
	
	for(c = list; c != NULL; c = next)
	{
		next = c->next;
		close(c->s[0]);
		close(c->s[1]);
		free(c);
	}
	
	return 0;
}

int FreeBalanceList(BALANCE_NODE *list)
{
	BALANCE_NODE *b = NULL, *next = NULL;
	
	if(list == NULL)
		return 1;
	
	for(b = list; b != NULL; b = next)
	{
		next = b->next;
		free(b);
	}
	
	return 0;
}

int FreeThreadList(THREAD_NODE *list)
{
	THREAD_NODE *t = NULL, *next = NULL;
	
	if(list == NULL)
		return 1;
	
	for(t = list; t != NULL; t = next)
	{
		next = t->next;
		free(t);
	}
	
	return 0;
}

int RemoveConnection(CONNECTION_NODE *con)
{
	CONNECTION_NODE *c = NULL, *next = NULL;
	
	if(!con)
		return 1;
	if(clist == NULL)
		return 1;
	if(clist == con)
	{
		next = clist->next;
		free(clist);
		clist = next;
	}
	else
	{
		for(c = clist; c->next != NULL && c->next != con; c = c->next);
		if(!c->next)
			return 1;
		next = c->next->next;
		free(c->next);
		c->next = next;
	}
	return 0;
}

int PrintConnectionList(CONNECTION_NODE *list)
{
	CONNECTION_NODE *c;
	
	if(!list)
		return 1;
	printf("IP\t\t:: Port\t\t:: Socket\t:: Node\t\t\t:: Port\t\t:: Socket\t:: Flags\n");
	for(c = list; c != NULL; c = c->next)
		printf("%u.%u.%u.%u\t:: %u\t:: %u\t\t:: %u.%u.%u.%u  \t:: %u    \t:: %u\t\t:: 0x%08x\n", *(unsigned char*)&c->addr.sin_addr.s_addr, *(((unsigned char*)&c->addr.sin_addr.s_addr) + 1), *(((unsigned char*)&c->addr.sin_addr.s_addr) + 2), *(((unsigned char*)&c->addr.sin_addr.s_addr) + 3), ntohs(c->addr.sin_port), c->s[0], *(unsigned char*)&c->bal->addr.sin_addr.s_addr, *(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 1), *(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 2), *(((unsigned char*)&c->bal->addr.sin_addr.s_addr) + 3), ntohs(c->bal->addr.sin_port), c->s[1], (unsigned int)c->flags);
	return 0;
}

int PrintBalanceList(BALANCE_NODE *list)
{
	BALANCE_NODE *b = NULL;
	
	if(!list)
		return 1;
	printf("IP\t\t:: Port\t\t:: Max\t:: Cur\t:: Total\t:: Flags\n");
	for(b = list; b != NULL; b = b->next)
		printf("%u.%u.%u.%u\t:: %u    \t:: %u\t:: %u\t:: %lu\t\t:: %08x\n", *(unsigned char*)(&b->addr.sin_addr.s_addr), *(((unsigned char*)&b->addr.sin_addr.s_addr) + 1), *(((unsigned char*)&b->addr.sin_addr.s_addr) + 2), *(((unsigned char*)&b->addr.sin_addr.s_addr) + 3), ntohs(b->addr.sin_port), b->max_load, b->cur_load, b->count, b->flags);
	return 0;
}

int PrintThreadList(THREAD_NODE *list)
{
	THREAD_NODE *t;
	
	if(!list)
		return 1;
#ifndef _NO_THREADS
	printf("Thread ID\t:: Accept\t:: Connection\t:: Watch\t:: Stdin\n");
#else
	printf("Accept\t:: Connection\t:: Watch\t:: Stdin\n");
#endif
	for(t = list; t != NULL; t = t->next)
#ifndef _NO_THREADS
		printf("%u\t\t:: %lu\t\t:: %lu\t\t:: %lu\t\t:: %lu\n", (unsigned int)t->tid, t->a, t->c, t->w, t->s);
#else
		printf("%lu\t\t:: %lu\t\t:: %lu\t\t:: %lu\n", t->a, t->c, t->w, t->s);
#endif
	return 0;
}

int StartWatch()
{
	int flag = 1;
	
	bdbg(5) printf("StartWatch(): Opening Watching socket.\n");
	if(!http.sin_port)
	{
		if(l.sin_port == htons(80))
			http.sin_port = htons(8080);
		else
			http.sin_port = htons(80);
	}
	if((w = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
	{
		bdbg(1) printf("StartWatch(): Failed to create socket.\n");
		return 1;
	}
	if(setsockopt(w, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) == -1)
	{
		bdbg(1) printf("StartWatch(): Failed to set socket options\n");
		close(w);
		return 1;
	}
	if(bind(w, (struct sockaddr*)&http, sizeof(struct sockaddr_in)) == -1)
	{
		bdbg(1) printf("StartWatch(): Failed to bind socket to port %d.\n", ntohs(http.sin_port));
		close(w);
		return 1;
	}
	if(listen(w, 5) == -1)
	{
		bdbg(1) printf("StartWatch(): Failed to listen to port %d.\n", ntohs(http.sin_port));
		close(w);
		return 1;
	}
	set(&flags, BAL_WATCH);
	return 0;
}
