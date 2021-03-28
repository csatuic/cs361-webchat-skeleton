
#include "csapp.h"

const char OK[] = "HTTP/1.1 200 OK\r\nContent-type: text/html; charset=UTF-8\r\n\r\n";

const char NOTOK[] = "HTTP/1.1 400 Bad Request\r\n\r\n";

const char stream[] =
    "HTTP/1.1 200 OK\r\n"
    "Cache-Control: no-transform\r\n"
    "Content-Type: text/event-stream\r\n"
    "Connection: keep-alive\r\n\r\n"
    "retry: 10000\n\n";

char indexhtml[4096];

typedef struct {
  char requestbuf[4096];
  int first_empty_byte;
} recv_buffer;

typedef struct {
  /* represents a pool of connected descriptors */
  int maxfd;                /* largest descriptor in read_set */
  fd_set read_set;          /* set of all active descriptors */
  fd_set ready_set;         /* subset of descriptors ready for reading  */
  int nready;               /* number of ready descriptors from select */
  int maxi;                 /* highwater index into client array */
  int clientfd[FD_SETSIZE]; /* set of active descriptors */
  recv_buffer protos[FD_SETSIZE];
  int receiving_events[FD_SETSIZE];  // flag: set 1 if client is listening for
                                     // chat messages, 0 otherwise
} pool;
void init_pool(int listenfd, pool *p);
void add_client(int connfd, pool *p);
void check_clients(pool *p);

void extract_message(char *src, char *dst) {
  // given an HTTP POST message containing a message, format it and
  // write it into dst. both buffers are 4096 bytes max.
  // move to beginning of message
  char *end_of_uri;
  char *plus;
  src += strlen("POST /speak?");
  strncpy(dst, "data: ", 4096);
  end_of_uri = strchr(src, ' ');
  strncpy(&dst[6], src, end_of_uri - src);
  strncat(dst, "\n\n", 3);
  // replace '+' with ' '
  for (; (plus = strchr(dst, '+')) != NULL; *plus = ' ')
    ;
}

// when you know you're done with a connection, close the socket,
// remove it from the set you're interested in reading from, and
// indicate that this offset in the pool is available.
void close_and_remove(pool *p, int offset) {
  Close(p->clientfd[offset]);
  FD_CLR(p->clientfd[offset], &p->read_set);
  p->clientfd[offset] = -1;
}

// courtesy Richard Stevens Unix Network Programming book.
ssize_t /* Write "n" bytes to a descriptor. */
writen(int fd, const void *vptr, size_t n) {
  size_t nleft;
  ssize_t nwritten;
  const char *ptr;
  ptr = vptr;
  nleft = n;
  while (nleft > 0) {
    if ((nwritten = write(fd, ptr, nleft)) <= 0) {
      if (nwritten < 0 && errno == EINTR)
        nwritten = 0; /* and call write() again */
      else
        return (-1); /* error */
    }
    nleft -= nwritten;
    ptr += nwritten;
  }
  return (n);
}

// this is very hacky, but it is good enough for our purposes.
typedef enum request_states {
  incomplete,
  root,
  speak,
  sse_listen
} request_type;

request_type parse_request(recv_buffer *ps) {
  // TODO: based on the bytes in the buffer, return a specific request type.
  // incomplete should be used when you can't tell what type of request this is,
  // so you have to wait for more bytes.
  return incomplete;
  // root should be used when you've received a complete request, and you know
  // it's for index.html.
  return root;
  // speak should be used when you've received a complete request, and you know
  // it's a POST message for the resource /speak.
  return speak;
  // sse_listen should be used when you've received a complete request, and you
  // know it's for the /listen endpoint.
  return sse_listen;
}

int main(int argc, char **argv) {
  int listenfd, connfd, port, filefd;
  socklen_t clientlen = sizeof(struct sockaddr_in);
  struct sockaddr_in clientaddr;
  static pool pool;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(0);
  }
  // read index into memory once to send to everyone
  filefd = open("index.html", O_RDONLY);
  read(filefd, indexhtml, 8192);
  close(filefd);

  port = atoi(argv[1]);
  // Open_listenfd calls socket(), bind(), and listen()
  listenfd = Open_listenfd(port);
  init_pool(listenfd, &pool);
  fprintf(stderr, "listening on http://localhost:%s\n", argv[1]);

  while (1) {
    /* Wait for listening/connected descriptor(s) to become ready */
    pool.ready_set = pool.read_set;
    pool.nready = Select(pool.maxfd + 1, &pool.ready_set, NULL, NULL, NULL);

    /* If listening descriptor ready, add new client to pool */
    if (FD_ISSET(listenfd, &pool.ready_set)) {
      connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      add_client(connfd, &pool);
    }
    // perform the relevant protocol steps for each FD that has bytes available
    check_clients(&pool);
  }
}

void init_pool(int listenfd, pool *p) {
  /* Initially, there are no connected descriptors */
  int i;
  p->maxi = -1;
  for (i = 0; i < FD_SETSIZE; i++) p->clientfd[i] = -1;

  /* Initially, listenfd is only member of select read set */
  p->maxfd = listenfd;
  FD_ZERO(&p->read_set);
  FD_SET(listenfd, &p->read_set);
}

void add_client(int connfd, pool *p) {
  int i;
  p->nready--;
  for (i = 0; i < FD_SETSIZE; i++) /* Find an available slot */
    if (p->clientfd[i] < 0) {
      /* Add connected descriptor to the pool */
      p->clientfd[i] = connfd;
      /* Add the descriptor to descriptor set */
      FD_SET(connfd, &p->read_set);
      // wipe buffer and indicate it is empty
      memset(p->protos[i].requestbuf, 0, 8192);
      p->protos[i].first_empty_byte = 0;
      // set state for stateful connections
      p->receiving_events[i] = 0;

      /* Update max descriptor and pool highwater mark */
      if (connfd > p->maxfd) p->maxfd = connfd;
      if (i > p->maxi) p->maxi = i;
      break;
    }
  if (i == FD_SETSIZE) /* Couldn't find an empty slot */
    app_error("add_client error: Too many clients");
}

void check_clients(pool *p) {
  int i, j, connfd, n, offset;
  char *end_of_uri;
  char *uri_ptr;
  char buf[MAXLINE];
  recv_buffer *ps;

  for (i = 0; (i <= p->maxi) && (p->nready > 0); i++) {
    connfd = p->clientfd[i];
    ps = &p->protos[i];
    offset = p->protos[i].first_empty_byte;

    if ((connfd > 0) && (FD_ISSET(connfd, &p->ready_set))) {
      p->nready--;
      // TODO: for the file descriptor that is ready, accumulate the received
      // bytes into the right receive buffer at the right offset, and keep
      // track of how many bytes have been received (so that you can keep
      // accumulating if more bytes come in).
      // Hint: don't forget to take care of the special case where the number
      // of bytes received is zero: that means that the client ended the
      // conversation (sent EOF).

      // TODO: based on the bytes received, move the protocol forward and
      // potentially send some bytes back to the client(s).
      switch (parse_request(ps)) {
        case incomplete:
          break;
        case root:
          // The server needs to respond to requests for the index by sending a
          // valid HTTP response along with the content of the index file.
          break;
        case speak:
          // The server needs to respond with an OK message, and send this message
          // to every currently listening client.
          break;
        case sse_listen:
          // Don't forget to add this socket to the "listening sockets that
          // should receive chat messages" list, and send back the relevant OK
          // message for a stream request.
          break;
        default:
          // If we recieve a full request but don't know what to do with it,
          // send back NOTOK and end the conversation.
          break;
      }
    }
  }
