
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <curl/curl.h>
#include <iostream>
#include <vector>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <mysql.h>
#include <errmsg.h>
#include <sys/resource.h>
#include "pqueue.h" 

#define BUFSIZE (1024*1024)   // maximum size of any datagram(16 bits the size of identifier)
#define TRUE 1
#define FALSE 0
#define default_low  1
#define default_high 1024

#define READBUF_CHUNK 4096

#define MAX_TIMEOUT 10   // In seconds
#define MAX_RETRIES 10
#define MAX_REDIRS  10

enum RequestsStatus {
	reqEmpty, reqPending,
	reqDnsQuery, reqDnsQuerying, 
	reqConnecting, reqTransfer, 
	reqCurl, reqCurlTransfer,
	reqComplete, reqCompleteError, reqError
};

volatile int verbose = 0;

#define MAX_OUTSTANDING_QUERIES (1024*256)
struct connection_query {
	int socket;
	unsigned long ip;
	unsigned short port;
	RequestsStatus status;
	unsigned char retries;
	unsigned char redirs;
	
	std::string vhost;
	std::string url;
	int tosend_max, tosend_offset;
	int received;
	char *inbuffer, *outbuffer;
	unsigned long start_time;
};

pqueue new_queries;
pqueue dns_queries;
pqueue curl_queries;
pqueue connection_queries;
pqueue completed_queries;
std::vector <connection_query*> active_connections;

volatile long long num_queued    = 0;
volatile long long num_completed = 0;

/*
 3 paths: 
     banner (new->connect->complete)
     http   (new->dns->connect->complete)
     https  (new->curl->complete)

  backpath: from any state to new_queries when error is found
 
 new_queries  ---> dns_queries  ---> connection_query  ---> completed_queries
               |                 ^                      ^
               ------------------!                      |
               |                                        |
               ----------> curl_queries  ---------------!
*/

connection_query connection_table[MAX_OUTSTANDING_QUERIES];

struct pollfd poll_desc[MAX_OUTSTANDING_QUERIES];

MYSQL *mysql_conn_select = 0;
MYSQL *mysql_conn_update = 0;
MYSQL *mysql_conn_update2 = 0;

void * database_dispatcher(void * args);
void * dns_dispatcher(void * args);
void * curl_dispatcher(void * args);
void mysql_initialize();

#define CONNECT_ERR(ret) (ret < 0 && errno != EINPROGRESS && errno != EALREADY && errno != EISCONN)
#define CONNECT_OK(ret) (ret >= 0 || (ret < 0 && (errno == EISCONN || errno == EALREADY)))
#define IOTRY_AGAIN(ret) (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))

volatile int adder_finish = 0;

int max_inflight = 100;
int max_dns_inflight = 50;
int max_curl_inflight = 5;

int setNonblocking(int fd) {
	int flags;
	/* If they have O_NONBLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
	/* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
	if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
		flags = 0;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
	/* Otherwise, use the old way of doing it */
	flags = 1;
	return ioctl(fd, FIOBIO, &flags);
#endif
}

std::string mysql_real_escape_std_string(MYSQL * c, const std::string s) {
	char tempb[s.size()*2+2];
	mysql_real_escape_string(c, tempb, s.c_str(), s.size());
	return std::string(tempb);
}

std::string generateHTTPQuery(const std::string vhost, const std::string path) {
	std::string p = (path == "") ? "/" : path;

	return "GET " + p + " HTTP/1.1\r\nHost: " + vhost +  \
		"\r\nUser-Agent: Mozilla/5.0 (Windows; U; Windows NT 5.1; rv:22.0) Gecko/20100101 Firefox/22.0\r\nConnection: close\r\n\r\n";
}

// Returns http[s]://domain.com/   given any URL under that website (base URL as its name indicates)
std::string base_url (const std::string & burl) {
	unsigned int s = 0;
	if (burl.substr(0,7) == "http://") s = 7;
	if (burl.substr(0,8) == "https://") s = 8;
	while (s < burl.size() && burl[s] != '/')
		s++;
	
	if (s == burl.size()) return burl + "/";
	return burl.substr(0,s+1);
}

std::string gethostname(const std::string & url) {
	std::string r = base_url(url);
	if (r.substr(0,7) == "http://")  r = r.substr(7);
	if (r.substr(0,8) == "https://") r = r.substr(8);
	
	size_t p = r.find("/");
	if (p == std::string::npos)
		return r;
	else
		return r.substr(0,p);
}
std::string getpathname(const std::string & url) {
	std::string r = base_url(url);
	if (r.substr(0,7) == "http://")  r = r.substr(7);
	if (r.substr(0,8) == "https://") r = r.substr(8);
	
	size_t p = r.find("/");
	if (p == std::string::npos)
		return "/";
	else
		return r.substr(p);
}

std::string parse_response(char * buffer, int size, const std::string current_url) {
	// Look for redirects only in the header
	buffer[size] = 0;
	char * limit = strstr(buffer,"\r\n\r\n");
	char * location = strcasestr(buffer,"Location:");

	if (limit == 0 || location == 0) return "";
	if ((uintptr_t)location > (uintptr_t)limit) return "";

	// We found a Location in the headers!
	location += 9;
	while (*location == ' ') location++;

	std::string temploc;
	while (*location != '\r' && *location != ' ' && (uintptr_t)location < (uintptr_t)limit)
		temploc += *location++;

	// Append a proper header in case it's just a relative URL
	if (temploc.substr(0,7) == "http://" || temploc.substr(0,8) == "https://") {
		return temploc;
	}
	else {
		return base_url(current_url) + temploc;
	}
}

void clean_entry(struct connection_query * q) {
	close(q->socket);
	q->vhost = "";
	q->url = "";
	if (q->inbuffer) free(q->inbuffer);
	if (q->outbuffer) free(q->outbuffer);
	q->status = reqEmpty;
}

int main(int argc, char **argv) {
	printf(
"  __      __  __      __  ______  ______  ______    \n"
" /\\ \\  __/\\ \\/\\ \\  __/\\ \\/\\__  _\\/\\__  _\\/\\__  _\\   \n"
" \\ \\ \\/\\ \\ \\ \\ \\ \\/\\ \\ \\ \\/_/\\ \\/\\/_/\\ \\/\\/_/\\ \\/   \n"
"  \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\   \\ \\ \\   \\ \\ \\   \n"
"   \\ \\ \\_/ \\_\\ \\ \\ \\_/ \\_\\ \\ \\_\\ \\__ \\ \\ \\   \\ \\ \\  \n"
"    \\ `\\___x___/\\ `\\___x___/ /\\_____\\ \\ \\_\\   \\ \\_\\ \n"
"     '\\/__//__/  '\\/__//__/  \\/_____/  \\/_/    \\/_/ \n"
"                                                    \n"
"         World Wide Internet Takeover Tool          \n"
"                Web/Banner crawler                  \n"  );


	fprintf(stderr, "Using about %u memory mega-bytes table\n", sizeof(connection_table)>>20);
	
	if ( argc < 2 ) {
		fprintf(stderr, "Usage: %s http   [max-requests]\n", argv[0]);
		fprintf(stderr, "Usage: %s banner [max-requests]\n", argv[0]);
		exit(1);
	}
	if (argc == 3)
		max_inflight = atoi(argv[2]);
	
	struct rlimit limit; limit.rlim_cur = MAX_OUTSTANDING_QUERIES*1.1f; limit.rlim_max = MAX_OUTSTANDING_QUERIES*1.6f;
	if (setrlimit(RLIMIT_NOFILE, &limit)<0) {
		fprintf(stderr, "setrlimit failed!\n");
		exit(1);
	}
	
	int bannercrawl = (strcmp("banner",argv[1]) == 0);
	
	// Initialize structures
	for (unsigned int i = 0; i < sizeof(connection_table)/sizeof(connection_table[0]); i++)
		clean_entry(&connection_table[i]);
	
	signal(SIGPIPE, SIG_IGN);
	mysql_initialize();
	
	
	// Init queues
	pqueue_init(&new_queries);
	pqueue_init(&dns_queries);
	pqueue_init(&curl_queries);
	pqueue_init(&connection_queries);
	pqueue_init(&completed_queries);
	
	// Start!
	pthread_t db;
	pthread_create (&db, NULL, &database_dispatcher, &bannercrawl);

	pthread_t dns_workers[max_dns_inflight];
	pthread_t curl_workers[max_curl_inflight];
	if (!bannercrawl) {
		for (int i = 0; i < max_dns_inflight; i++)
			pthread_create (&dns_workers[i], NULL, &dns_dispatcher, 0);
		for (int i = 0; i < max_curl_inflight; i++)
			pthread_create (&curl_workers[i], NULL, &curl_dispatcher, 0);
	}

	int db_end = 0;
	// Infinite loop: query IP/Domain blocks
	const char * query = "SELECT `host` FROM `virtualhosts` WHERE (`head` IS null OR `index` IS null)";
	if (bannercrawl) query = "SELECT `ip`, `port` FROM `services` WHERE `head` IS null";
	mysql_query(mysql_conn_select, query);
	MYSQL_RES *result = mysql_store_result(mysql_conn_select);
	
	// Do this while DB has data and we have inflight requests
	while (num_queued != num_completed || !db_end) {
	
		printf("\rQueues: New %d Completed %d DNS %d Curl %d Connection %d  TOTAL INFLIGHT %lld     ", 
			pqueue_size(&new_queries),
			pqueue_size(&completed_queries),pqueue_size(&dns_queries),pqueue_size(&curl_queries),
			pqueue_size(&connection_queries)+active_connections.size(),
			num_queued - num_completed);
		fflush(stdout);
		
		// Generate queries and generate new connections
		if (num_queued - num_completed < max_inflight && !db_end) {
			long long na = num_queued - num_completed;
			//printf("\rAdding more jobs to queue, num active: %lld ...   ", na); fflush(stdout);

			db_end = 1;
			MYSQL_ROW row;
			while ((row = mysql_fetch_row(result))) {
				db_end = 0;
				struct connection_query cquery;
				cquery.ip = 0;
				cquery.retries = cquery.redirs = 0;
				cquery.tosend_max = cquery.tosend_offset = 0;
				cquery.received = 0;
				cquery.inbuffer = (char*)malloc(READBUF_CHUNK+32);
				memset(cquery.inbuffer,0,READBUF_CHUNK+32);
				cquery.outbuffer = 0;
				cquery.vhost = "";
				cquery.url = "";
				cquery.socket = socket(AF_INET, SOCK_STREAM, 0);
				cquery.start_time = time(0);
				cquery.status = reqPending;
				setNonblocking(cquery.socket);
			
				if (bannercrawl) {
					cquery.ip = atoi(row[0]);
					cquery.port = atoi(row[1]);
				}else{
					std::string vhost = std::string(row[0]);
					cquery.outbuffer = strdup(generateHTTPQuery(vhost,"/").c_str());
					cquery.tosend_max = strlen(cquery.outbuffer);
					cquery.port = 80;
					cquery.vhost = vhost;
					cquery.url = "http://" + vhost + "/";
					if (verbose)
						std::cout << "Query " << cquery.url << std::endl;
				}

				// Put it in a slot
				int i;
				for (i = 0; i < MAX_OUTSTANDING_QUERIES; i++) {
					if (connection_table[i].status == reqEmpty) {
						connection_table[i] = cquery;
						num_queued++;
						
						// Now queue it as a new request
						pqueue_push(&new_queries, &connection_table[i]);
						break;
					}
				}

				// Do not full queues!
				if (num_queued - num_completed >= max_inflight)
					break;
			}
		}
		
		// Read replies from DNS
		connection_query * icq = (connection_query*)pqueue_pop_nonb(&connection_queries);
		while (icq) {
			icq->status = reqConnecting;
			active_connections.push_back(icq);
			icq = (connection_query*)pqueue_pop_nonb(&connection_queries);
		}
		
		// Read requests from the new queue and cathegorize them
		icq = (connection_query*)pqueue_pop_nonb(&new_queries);
		while (icq) {
			// It may be new or a respin, check retries and redirs
			if (icq->retries++ >= MAX_RETRIES) {
				// We are done with this guy, mark is as false ready
				icq->received = 0;
				icq->status = reqCompleteError;
				pqueue_push(&completed_queries, icq);
			}
			else {
				// Good query, send it to the best suited queue
				if (bannercrawl)
					icq->status = reqConnecting;
				else{
					icq->status = (icq->url.substr(0,7) == "http://") ? reqDnsQuery : reqCurl;
					if (verbose)
						std::cout << "Query " << icq->url << std::endl;
				}
				if (icq->status == reqDnsQuery)   pqueue_push(&dns_queries, icq);
				if (icq->status == reqCurl)       pqueue_push(&curl_queries, icq);
				if (icq->status == reqConnecting) active_connections.push_back(icq);
			}
						
			icq = (connection_query*)pqueue_pop_nonb(&new_queries);
		}
		
		// Connection loop, just walk the connection queue
		int num_fd_active = 0;
		for (unsigned int i = 0; i < active_connections.size(); i++) {
			connection_query * cq = active_connections[i];
			if (cq->status == reqConnecting) {
				struct sockaddr_in sin;
				sin.sin_port = htons(cq->port);
				sin.sin_addr.s_addr = htonl(cq->ip);
				sin.sin_family = AF_INET;
			
				int res = connect(cq->socket,(struct sockaddr *)&sin,sizeof(sin));
				if (CONNECT_ERR(res)) {
					// Drop connection!
					cq->status = reqError;
				}
				else if (CONNECT_OK(res)) {
					cq->status = reqTransfer;
				}
			}
			if (cq->status == reqTransfer) {
				// Try to send data (if any)
				if (cq->tosend_offset < cq->tosend_max) {
					int sent = write(cq->socket,&cq->outbuffer[cq->tosend_offset],cq->tosend_max-cq->tosend_offset);
					if (sent > 0) {
						cq->tosend_offset += sent;
					}
					else if (!IOTRY_AGAIN(sent)) {
						cq->status = reqError;
					}
				}
			
				// Try to receive data
				int rec = read(cq->socket, &cq->inbuffer[cq->received], READBUF_CHUNK);
				if (cq->received > BUFSIZE) rec = 0; // Force stream end at BUFSIZE
				if (rec > 0) {
					cq->received += rec;
					cq->inbuffer = (char*)realloc(cq->inbuffer, cq->received + READBUF_CHUNK + 32);
					cq->inbuffer[cq->received] = 0; // Add zero to avoid strings running out the buffer
				}
				else if (rec == 0) {
					// End of data. OK if we sent all the data only
					if (cq->tosend_offset < cq->tosend_max) 
						cq->status = reqError; // Disconnect, but ERR!
					else
						cq->status = reqComplete; // Disconnect, OK!
				}
				else if (!IOTRY_AGAIN(rec)) {
					// Connection error!
					cq->status = reqError;
				}
			}
			
			// Check timeout, is OK for banner, not for HTTP
			if (time(0) - cq->start_time > MAX_TIMEOUT) {
				if (bannercrawl && cq->received > 0)
					cq->status = reqComplete;
				else
					cq->status = reqError;
			}
			
			// Setup poll
			if (cq->status == reqConnecting || cq->status == reqTransfer) {
				int mask = POLLIN;
				mask |= (cq->tosend_offset < cq->tosend_max) ? (POLLOUT|POLLERR) : 0;

				poll_desc[num_fd_active].fd = cq->socket;
				poll_desc[num_fd_active].events = mask;
				poll_desc[num_fd_active].revents = 0;
				num_fd_active++;
			}

			// Remove err/ok from list, they are queued after this
			if (cq->status == reqError || cq->status == reqComplete) {
				active_connections.erase(active_connections.begin()+i);
				i--;
			}
			// Handle error (respin) and complete (send to complete)
			if (cq->status == reqError) {
				cq->tosend_offset = 0;
				cq->received = 0;
				cq->start_time = time(0);
					
				// Put it again in the new_queue
				pqueue_push(&new_queries, cq);
			}
			if (cq->status == reqComplete) {
				// Put it in the completed queue
				pqueue_push(&completed_queries, cq);
			}
		}
		
		// Waiting. Poll for sockets
		poll(poll_desc, num_fd_active, 2000);
	}
	
	printf("No more things to crawl, waiting for threads...\n");
	pqueue_release(&dns_queries);
	pqueue_release(&curl_queries);
	pqueue_release(&completed_queries);
	adder_finish = 1;
	mysql_close(mysql_conn_select);
	
	pthread_join(db, NULL);
	if (!bannercrawl) {
		for (int i = 0; i < max_dns_inflight; i++)
			pthread_join (dns_workers[i], NULL);
		for (int i = 0; i < max_curl_inflight; i++)
			pthread_join (curl_workers[i], NULL);
	}
}

void db_reconnect(MYSQL ** c) {
	if (*c)
		mysql_close(*c);

	char *server = getenv("MYSQL_HOST");
	char *user = getenv("MYSQL_USER");
	char *password = getenv("MYSQL_PASS");
	char *database = getenv("MYSQL_DB");
	
	*c = mysql_init(NULL);
	// Eanble auto-reconnect, as some versions do not enable it by default
	my_bool reconnect = 1;
	mysql_options(*c, MYSQL_OPT_RECONNECT, &reconnect);
	if (!mysql_real_connect(*c, server, user, password, database, 0, NULL, 0)) {
		fprintf(stderr, "User %s Pass %s Host %s Database %s\n", user, password, server, database);
		exit(1);
	}
}

void mysql_initialize() {
	/* Connect to database */
	printf("Connecting to mysqldb...\n");
	db_reconnect(&mysql_conn_select);
	db_reconnect(&mysql_conn_update);
	db_reconnect(&mysql_conn_update2);
	printf("Connected!\n");
}

void separate_body(char * buffer, int size, char ** buf1, char ** buf2, int * len1, int * len2) {
	buffer[size] = 0;
	char * bodybody = strstr(buffer,"\r\n\r\n");
	if (!bodybody) bodybody = &buffer[size];

	*len1 = ((uintptr_t)bodybody - (uintptr_t)buffer);
	*len2 = size - (*len1) - 4;
	if (*len2 < 0) *len2 = 0;

	bodybody += 4;
	*buf1 = buffer;
	*buf2 = bodybody;
}

int parse_hex(const char * cptr) {
	int ret = 0;
	int valid;
	do {
		valid = 1;
		
		if (*cptr >= '0' && *cptr <= '9')
			ret = (ret << 4) | (*cptr - '0');
		else if (*cptr >= 'a' && *cptr <= 'f')
			ret = (ret << 4) | (*cptr - 'a' + 10);
		else if (*cptr >= 'A' && *cptr <= 'F')
			ret = (ret << 4) | (*cptr - 'A' + 10);
		else
			valid = 0;
		cptr++;

	} while (valid);
	return ret;
}

// In case of "Transfer-Encoding: chunked" de-chunk the HTTP body
void dechunk_http(char * buffer, int * size) {
	char * head, * body;
	int head_len, body_len;
	separate_body(buffer, *size, &head, &body, &head_len, &body_len);
	if (body_len == 0) return;
	
	// Look for Transfer-encoding
	char * chunk = strcasestr(head, "Transfer-encoding");
	if (!chunk || ((uintptr_t)chunk > (uintptr_t)body) ) return;
	// Look for the "chunked" word before the next break line
	char * chunkend = strstr(chunk, "\r\n");
	if (!chunkend || ((uintptr_t)chunkend > (uintptr_t)body) ) return;
	
	char * chunked = strcasestr(chunk, "chunked");
	if (!chunked || ((uintptr_t)chunked > (uintptr_t)body) ) return;
	
	// Now proceed to dechunk the body
	char * newbuffer = (char*)malloc(*size+32);
	memcpy(newbuffer, buffer, head_len+4);
	char * newbody = &newbuffer[head_len+4];
	int newlen = head_len+4;
	while ( (uintptr_t)body < (uintptr_t)&buffer[*size] ) {
		int len = parse_hex(body);
		if (len + newlen > *size) break; // Overflow, should not happen
		if (len == 0) break;  // Last chunk
		body = strstr(body, "\r\n");
		if (!body) break;
		body += 2;
		memcpy(newbody, body, len);
		newbody += len;
		body += (len+2);  // Skip \r\n
		newlen += len;
	}
	
	// Copy and free!
	memcpy(buffer, newbuffer, newlen);
	*size = newlen;
	free(newbuffer);
}

// Wait for DNS requests, process them one at a time and store the IP back
void * dns_dispatcher(void * args) {
	connection_query * cquery = (connection_query*)pqueue_pop(&dns_queries);

	while (cquery) {
		assert(cquery->status == reqDnsQuery);
		cquery->status = reqDnsQuerying;

		struct addrinfo *result;
		std::string tosolve = gethostname(cquery->url);
		if (getaddrinfo(tosolve.c_str(), NULL, NULL, &result) == 0) {
			unsigned long ip = ntohl(((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr);
			cquery->ip = ip;
			cquery->start_time = time(0);
			if (verbose)
				std::cout << "Resolved " << tosolve << " to " << ip << std::endl;
			
			pqueue_push(&connection_queries, cquery);
		}
		else {
			pqueue_push(&new_queries, cquery);
		}
		
		cquery = (connection_query*)pqueue_pop(&dns_queries);
	}
	return 0;
}

// Walk the table from time to time and insert results into the database
void * database_dispatcher(void * args) {
	int bannercrawl = *(int*)args;
	char sql_query[BUFSIZE*2];
	unsigned long long num_processed = 0;
	
	connection_query * cquery = (connection_query*)pqueue_pop(&completed_queries);

	while (cquery) {
		assert(cquery->status == reqComplete || cquery->status == reqCompleteError);

		// Analyze the response, maybe we find some redirects
		dechunk_http(cquery->inbuffer,&cquery->received);
		std::string newloc = parse_response((char*)cquery->inbuffer, cquery->received, cquery->url);
		
		if (newloc != "" && cquery->redirs < MAX_REDIRS && !bannercrawl) {
			// Redirect! Setup the query again
			cquery->retries = 0;
			cquery->redirs++;
			cquery->tosend_offset = 0;
			cquery->received = 0;
			cquery->inbuffer = (char*)realloc(cquery->inbuffer,READBUF_CHUNK+32);
			memset(cquery->inbuffer,0,READBUF_CHUNK+32);
			close(cquery->socket);
			cquery->socket = socket(AF_INET, SOCK_STREAM, 0);
			cquery->start_time = time(0);
			setNonblocking(cquery->socket);
			cquery->status = reqPending;

			free(cquery->outbuffer);
			std::string path = getpathname(newloc);
			std::string host = gethostname(newloc);

			cquery->outbuffer = strdup(generateHTTPQuery(host, path).c_str());
			cquery->tosend_max = strlen(cquery->outbuffer);
			cquery->url = newloc;
			if (verbose)
				std::cout << "Query (redir) " << cquery->url << std::endl;

			pqueue_push(&new_queries, cquery);
		}
		else{
			if (bannercrawl) {
				char tempb[cquery->received*2+2];
				mysql_real_escape_string(mysql_conn_update, tempb, cquery->inbuffer, cquery->received);
				sprintf(sql_query,"UPDATE `services` SET `head`='%s' WHERE `ip`=%lu AND `port`=%d\n",
					tempb, cquery->ip, cquery->port);
			}else{
				int eflag = cquery->status == reqCompleteError ? 0x80 : 0;
				char tempb [cquery->received*2+2];
				char tempb_[cquery->received*2+2];
				int len1, len2; char *p1, *p2;
				separate_body(cquery->inbuffer, cquery->received, &p1, &p2, &len1, &len2);

				mysql_real_escape_string(mysql_conn_update, tempb,  p1, len1);
				mysql_real_escape_string(mysql_conn_update, tempb_, p2, len2);

				std::string hostname = mysql_real_escape_std_string(mysql_conn_update,cquery->vhost);
				std::string url      = mysql_real_escape_std_string(mysql_conn_update,cquery->url);

				sprintf(sql_query, "UPDATE virtualhosts SET `index`='%s',`head`='%s',`url`='%s',`status`=`status`|%d WHERE  `host`=\"%s\";", tempb_, tempb, url.c_str(), eflag, hostname.c_str());
			}
			num_processed++;
			if (mysql_query(mysql_conn_update,sql_query)) {
				if (mysql_errno(mysql_conn_update) == CR_SERVER_GONE_ERROR) {
					printf("Reconnect due to connection drop\n");
					db_reconnect(&mysql_conn_update);
				}
			}
			if (verbose)
				std::cout << "Completed " << cquery->url << std::endl;

			// Entry cleanup
			clean_entry(cquery);
			num_completed++;  // Allow one more to come in
		}
		
		cquery = (connection_query*)pqueue_pop(&completed_queries);
	}
	
	printf("End of crawl, quitting!\n");
	printf("Inserted %llu registers\n", num_processed);

	mysql_close(mysql_conn_update);
	return 0;
}


// CURL worker thread
struct http_query {
	char * buffer;
	int received;
};
static size_t curl_fwrite(void *buffer, size_t size, size_t nmemb, void *stream);

void * curl_dispatcher(void * args) {
	connection_query * cquery = (connection_query*)pqueue_pop(&curl_queries);

	while (cquery) {
		assert(cquery->status == reqCurl);
		cquery->status = reqCurlTransfer;

		struct http_query hq;
		memset(&hq,0,sizeof(hq));
		hq.buffer = (char*)malloc(32);

		CURL * curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_fwrite);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &hq);
		curl_easy_setopt(curl, CURLOPT_URL, cquery->url.c_str());

		if (verbose)
			std::cout << "cURL Fetch " << cquery->url << std::endl;
		CURLcode res = curl_easy_perform(curl);
		
		// Move data
		cquery->inbuffer = hq.buffer;
		cquery->received = hq.received;

		if(CURLE_OK == res) {
			cquery->status = reqComplete;
			pqueue_push(&completed_queries, cquery);
		}
		else {
			cquery->status = reqError;
			pqueue_push(&new_queries, cquery);
		}

		curl_easy_cleanup(curl);
		
		// Next request!
		cquery = (connection_query*)pqueue_pop(&curl_queries);
	}
	
	std::cout << "Closing cURL thread" << std::endl;
	return 0;
}


static size_t curl_fwrite(void *buffer, size_t size, size_t nmemb, void *stream) {
	struct http_query * q = (struct http_query *)stream;

	q->buffer = (char*)realloc(q->buffer, size*nmemb + q->received + 32);
	char *bb = q->buffer;

	memcpy(&bb[q->received], buffer, size*nmemb);
	q->received += size*nmemb;

	//if (q->received > MAX_BUFFER_SIZE)
	//	return -1;  // Ops!

	return size*nmemb;
}



