/* commands_tunnel.c
 * By Ron Bowes
 * Created December, 2015
 *
 * See LICENSE.md
 *
 * Despite the name, this isn't realllly a header file. I moved some of
 * the functions into it to keep them better organized.
 */

static uint32_t g_tunnel_id = 0;

typedef struct
{
  uint32_t          tunnel_id;
  int               s;
  driver_command_t *driver;
  uint32_t          connect_request_id;
} tunnel_t;

static void send_and_free(buffer_t *outgoing_data, command_packet_t *out)
{
  uint8_t          *out_data = NULL;
  size_t            out_length;

  out_data = command_packet_to_bytes(out, &out_length);
  buffer_add_bytes(outgoing_data, out_data, out_length);
  safe_free(out_data);
  command_packet_destroy(out);
}

static SELECT_RESPONSE_t tunnel_data_in(void *group, int s, uint8_t *data, size_t length, char *addr, uint16_t port, void *param)
{
  tunnel_t         *tunnel   = (tunnel_t*) param;
  command_packet_t *out      = NULL;

  out = command_packet_create_tunnel_data_request(request_id(), tunnel->tunnel_id, data, length);
  send_and_free(tunnel->driver->outgoing_data, out);

  return SELECT_OK;
}

static SELECT_RESPONSE_t tunnel_closed(void *group, int s, void *param)
{
  tunnel_t         *tunnel   = (tunnel_t*) param;
  command_packet_t *out      = NULL;

  printf("Socket was closed on the server side of tunnel %d!\n", tunnel->tunnel_id);

  /* Queue up a packet letting the server know the connection is gone. */
  out = command_packet_create_tunnel_close_request(request_id(), tunnel->tunnel_id);
  send_and_free(tunnel->driver->outgoing_data, out);

  /* Remove the tunnel from the linked list of tunnels. */
  ll_remove(tunnel->driver->tunnels, ll_32(tunnel->tunnel_id));
  safe_free(tunnel);

  /* Close the socket. */
  tcp_close(tunnel->s);

  return SELECT_REMOVE;
}

static SELECT_RESPONSE_t tunnel_error(void *group, int s, int err, void *param)
{
  tunnel_t         *tunnel   = (tunnel_t*) param;
  command_packet_t *out      = NULL;

  printf("Error in tunnel %d! Errno: %d\n", tunnel->tunnel_id, err);

  /* Queue up a packet letting the server know the connection is gone. */
  out = command_packet_create_tunnel_close_request(request_id(), tunnel->tunnel_id);
  send_and_free(tunnel->driver->outgoing_data, out);

  /* Remove the tunnel from the linked list of tunnels. */
  ll_remove(tunnel->driver->tunnels, ll_32(tunnel->tunnel_id));
  safe_free(tunnel);

  /* Close the socket. */
  tcp_close(tunnel->s);

  return SELECT_REMOVE;
}

static SELECT_RESPONSE_t tunnel_ready(void *group, int s, void *param)
{
  tunnel_t         *tunnel   = (tunnel_t*) param;
  command_packet_t *out      = NULL;

  printf("Tunnel %d successfully connected!\n", tunnel->tunnel_id);

  /* Queue up a packet letting the server know the connection is ready. */
  out = command_packet_create_tunnel_connect_response(tunnel->connect_request_id, tunnel->tunnel_id);
  send_and_free(tunnel->driver->outgoing_data, out);

  return SELECT_OK;
}

static command_packet_t *handle_tunnel_connect(driver_command_t *driver, command_packet_t *in)
{
  command_packet_t *out    = NULL;
  tunnel_t         *tunnel = NULL;

  if(!in->is_request)
    return NULL;

  LOG_WARNING("Connecting to %s:%d...", in->r.request.body.tunnel_connect.host, in->r.request.body.tunnel_connect.port);

  tunnel = (tunnel_t*)safe_malloc(sizeof(tunnel_t));
  tunnel->tunnel_id          = g_tunnel_id++;
  tunnel->connect_request_id = in->request_id;
  tunnel->driver             = driver;
  tunnel->s                  = tcp_connect_options(in->r.request.body.tunnel_connect.host, in->r.request.body.tunnel_connect.port, TRUE);

  printf("s = %d\n", tunnel->s);

  if(tunnel->s == -1)
  {
    out = command_packet_create_error_response(in->request_id, TUNNEL_STATUS_FAIL, "The dnscat2 client couldn't connect to the remote host!");
  }
  else
  {
    /* Add the driver to the global list. */
    ll_add(driver->tunnels, ll_32(tunnel->tunnel_id), tunnel);

    select_group_add_socket(driver->group, tunnel->s, SOCKET_TYPE_STREAM, tunnel);
    select_set_recv(driver->group, tunnel->s, tunnel_data_in);
    select_set_closed(driver->group, tunnel->s, tunnel_closed);
    select_set_ready(driver->group, tunnel->s, tunnel_ready);
    select_set_error(driver->group, tunnel->s, tunnel_error);
  }

  return out;
}

static command_packet_t *handle_tunnel_data(driver_command_t *driver, command_packet_t *in)
{
  /* TODO: Find socket by tunnel_id */
  tunnel_t *tunnel = (tunnel_t *)ll_find(driver->tunnels, ll_32(in->r.request.body.tunnel_data.tunnel_id));
  if(!tunnel)
  {
    LOG_ERROR("Couldn't find tunnel: %d", in->r.request.body.tunnel_data.tunnel_id);
    return NULL;
  }
  printf("Received data to tunnel %d (%zd bytes)\n", in->r.request.body.tunnel_data.tunnel_id, in->r.request.body.tunnel_data.length);
  tcp_send(tunnel->s, in->r.request.body.tunnel_data.data, in->r.request.body.tunnel_data.length);

  return NULL;
}

static command_packet_t *handle_tunnel_close(driver_command_t *driver, command_packet_t *in)
{
  tunnel_t *tunnel = (tunnel_t *)ll_remove(driver->tunnels, ll_32(in->r.request.body.tunnel_data.tunnel_id));

  if(!tunnel)
  {
    LOG_WARNING("The server tried to close a tunnel that we don't know about: %d", in->r.request.body.tunnel_data.tunnel_id);
    return NULL;
  }

  select_group_remove_socket(driver->group, tunnel->s);
  tcp_close(tunnel->s);
  LOG_WARNING("Closed tunnel %d", tunnel->tunnel_id);
  safe_free(tunnel);

  return NULL;
}

