/* exthwmod.h: external hw module
   Copyright (c) 2023 XJ

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   E-mail: 

*/

#include <config.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "infrastructure/startup_manager.h"
#include "exthwmod.h"
#include "fuse.h"
#include "ui/ui.h"

const exhw_socket_t ay_socket_invalid = INVALID_SOCKET;
const int ay_socket_EBADF = WSAENOTSOCK;

struct exhw_module_t {
  SOCKET self_socket;
  libspectrum_word port;
};

static exhw_module_t *exthwmod = NULL;

int
ay_socket_close( exhw_socket_t fd )
{
  return closesocket( fd );
}

int ay_socket_get_error( void )
{
  return WSAGetLastError();
}

const char *
ay_socket_get_strerror( void )
{
  static TCHAR buffer[256];
  TCHAR *ptr;
  DWORD msg_size;

  /* get description of last winsock error */
  msg_size = FormatMessage( 
               FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
               NULL, WSAGetLastError(),
               MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
               buffer, ARRAY_SIZE( buffer ), NULL );

  if( !msg_size ) return NULL;

  /* skip 'new line' like chars */
  for( ptr = buffer; *ptr; ptr++ ) {
    if( ( *ptr == '\r' ) || ( *ptr == '\n' ) ) {
      *ptr = '\0';
      break;
    }
  }

  return (const char *)buffer;
}

exhw_socket_t exthw_module_get_read_fd( exhw_module_t *self )
{
  return self->self_socket;
}

void exthw_module_wake( exhw_module_t *self )
{
  struct sockaddr_in sa;

  memset( &sa, 0, sizeof(sa) );
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  sa.sin_port = htons( self->port );

  sendto( self->self_socket, NULL, 0, 0, (struct sockaddr*)&sa, sizeof(sa) );
}

void exthw_module_discard_data( exhw_module_t *self )
{
  ssize_t bytes_read;
  struct sockaddr_in sa;
  socklen_t sa_length = sizeof(sa);
  static char bitbucket[0x100];

  do {
    /* Socket is non blocking, so we can do this safely */
    bytes_read = recvfrom( self->self_socket, bitbucket, sizeof( bitbucket ),
                           0, (struct sockaddr*)&sa, &sa_length );
  } while( bytes_read != -1 );
}

static int ay_selfpipe_test( exhw_module_t *self )
{
  fd_set readfds;
  int active;
  struct timeval tv = { 1, 0 };

  /* Send testing packet */
  exthw_module_wake( self );

  /* Safe reading from control socket */
  FD_ZERO( &readfds );
  FD_SET( self->self_socket, &readfds );
  active = select( 0, &readfds, NULL, NULL, &tv );
  if( active == 0 || active == ay_socket_invalid ) {
    return -1;
  }

  /* Discard testing packet */
  if( FD_ISSET( self->self_socket, &readfds ) ) {
    exthw_module_discard_data( self );
  }

  return 0;
}
void ay_wsa_init( void )
{
  WORD wVersionRequested;
  WSADATA wsaData;
  int error;

  wVersionRequested = MAKEWORD( 2, 2 );
  error = WSAStartup( wVersionRequested, &wsaData );
  if( error ) {
    ui_error( UI_ERROR_ERROR, "%s:%d: error %d from WSAStartup()", __FILE__,
              __LINE__, error );
    fuse_abort();
  }

  if( LOBYTE( wsaData.wVersion ) != 2 || HIBYTE( wsaData.wVersion ) != 2 ) {
    ui_error( UI_ERROR_ERROR,
              "%s:%d: unexpected version 0x%02x from WSAStartup()",
              __FILE__, __LINE__, wsaData.wVersion );
    fuse_abort();
  }
}

exhw_module_t * exthw_module_alloc( void )
{
  unsigned long mode = 1;
  struct sockaddr_in sa;
  socklen_t sa_len = sizeof(sa);

  exhw_module_t *self =
    libspectrum_new( exhw_module_t, 1 );
  
  self->self_socket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
  if( self->self_socket == ay_socket_invalid ) {
    ui_error( UI_ERROR_ERROR,
              "%s: %d: failed to open socket; errno %d: %s\n",
              __FILE__, __LINE__, ay_socket_get_error(),
              ay_socket_get_strerror() );
    fuse_abort();
  }

  /* Set nonblocking mode */
  if( ioctlsocket( self->self_socket, FIONBIO, &mode ) == -1 ) {
    ui_error( UI_ERROR_ERROR, 
              "%s: %d: failed to set socket nonblocking; errno %d: %s\n",
              __FILE__, __LINE__, ay_socket_get_error(),
              ay_socket_get_strerror() );
    fuse_abort();
  }

  memset( &sa, 0, sizeof(sa) );
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

  if( bind( self->self_socket, (struct sockaddr*)&sa, sizeof(sa) ) == -1 ) {
    ui_error( UI_ERROR_ERROR,
              "%s: %d: failed to bind socket; errno %d: %s\n",
              __FILE__, __LINE__, ay_socket_get_error(),
              ay_socket_get_strerror() );
    fuse_abort();
  }

  /* Get ephemeral port number */
  if( getsockname( self->self_socket, (struct sockaddr *)&sa, &sa_len ) == -1 ) {
    ui_error( UI_ERROR_ERROR,
              "%s: %d: failed to get socket name; errno %d: %s\n",
              __FILE__, __LINE__, ay_socket_get_error(),
              ay_socket_get_strerror() );
    fuse_abort();
  }

  self->port = ntohs( sa.sin_port );
 
  /* Test communications in order to detect blocking firewalls */
  if( ay_selfpipe_test( self ) == -1 ) {
    ui_error( UI_ERROR_ERROR,
              "Networking: failed to test internal communications" );
    fuse_abort();
  }

  return self;
}

void exthw_module_senday(char areg, char adata)
{
  if (!exthwmod) return;
  struct sockaddr_in sa;

  memset( &sa, 0, sizeof(sa) );
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  sa.sin_port = htons( 9101 );
  
  //char server_message[6];
  //server_message[0]=hex_chars[ ( areg & 0xF0 ) >> 4 ];
  //server_message[1]=hex_chars[ ( areg & 0x0F ) >> 0 ];
  //server_message[2]=hex_chars[ ( adata & 0xF0 ) >> 4 ];
  //server_message[3]=hex_chars[ ( adata & 0x0F ) >> 0 ];
  //server_message[2]=13;
  //server_message[3]=10;
  char server_message[2];
  server_message[0]=areg;
  server_message[1]=adata;
  sendto( exthwmod->self_socket, server_message, sizeof(server_message), 0, (struct sockaddr*)&sa, sizeof(sa) );
}

void exthw_module_sendlog(char adata){
	exthw_module_senday(0xff,adata);
}

void exthw_module_sendcmd(char acmd){
   exthw_module_senday(0xfe,acmd);
}

void exthwmod_aymute(void){
  for( int i = 8; i<=10; i++ ) {
	exthw_module_senday(i, 0);
  }
}

void exthw_module_free(void)
{
  ay_socket_close( exthwmod->self_socket );
  libspectrum_free( exthwmod );
}

static int exthwmod_init( void *context )
{
  ay_wsa_init();
  exthwmod = exthw_module_alloc();
  exthwmod_aymute();
  exthw_module_sendlog(1);
  return 0;
}

void exthwmod_end( void )
{
  exthwmod_aymute();
  exthw_module_sendlog(255);
  exthw_module_free();
  WSACleanup();
}

void exthw_register_startup( void )
{  
  startup_manager_module dependencies[] = {
    //STARTUP_MANAGER_MODULE_DEBUGGER,
    //STARTUP_MANAGER_MODULE_MEMORY,
    //STARTUP_MANAGER_MODULE_SOUND,
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_EXTHW, dependencies,
                            ARRAY_SIZE( dependencies ), exthwmod_init, NULL,
                            exthwmod_end);
}

