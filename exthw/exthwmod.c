/* exthwmod.c: external hw module (real AY chip output via UDP or COM)
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

#include <stdio.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdlib.h>

#include "infrastructure/startup_manager.h"
#include "exthwmod.h"
#include "fuse.h"
#include "machine.h"
#include "settings.h"
#include "ui/ui.h"
#include "utils.h"

/* UDP packet: [reg, val]; reg 0xFF = log, 0xFE = cmd (AYSender protocol) */
#define EXTHW_REG_LOG 0xff
#define EXTHW_REG_CMD 0xfe

/* Serial protocol of AY_PlayerFuse.ino: 'w' reg val = write, 'r' x x = reset */
#define EXTHW_COM_WRITE 'w'
#define EXTHW_COM_RESET 'r'

/* Timed protocol v2, see exthw/README.md:
   'C' cpu_hz(u32) frame_t(u32) latency_ms(u8)   configuration
   'F' seq(u8) len(u16) data[len] xor(u8)        one frame of writes;
       entry = reg, val, dt (LEB128 T-states since previous entry)
   'X' 0 0                                       flush buffer + mute */
#define EXTHW_TIMED_CONFIG 'C'
#define EXTHW_TIMED_FRAME  'F'
#define EXTHW_TIMED_FLUSH  'X'

typedef enum exthw_mode_t {
  EXTHW_MODE_NONE = 0,
  EXTHW_MODE_UDP,
  EXTHW_MODE_COM,
} exthw_mode_t;

static exthw_mode_t exthw_mode = EXTHW_MODE_NONE;

static int wsa_started = 0;
static SOCKET udp_socket = INVALID_SOCKET;
static struct sockaddr_storage udp_addr;
static int udp_addr_len = 0;

static HANDLE com_handle = INVALID_HANDLE_VALUE;

/* Timed mode state: writes of the current frame, encoded */
static int timed = 0;
static unsigned char *frame_buf = NULL;
static size_t frame_len = 0, frame_alloc = 0;
static libspectrum_dword frame_last_t = 0;
static unsigned char frame_seq = 0;
static libspectrum_dword sent_cpu_hz = 0, sent_frame_t = 0;
static int sent_latency = -1;

static exthw_mode_t
exthw_parse_mode( const char *mode )
{
  if( !mode || !*mode ) return EXTHW_MODE_NONE;
  if( !strcasecmp( mode, "udp" ) ) return EXTHW_MODE_UDP;
  if( !strcasecmp( mode, "com" ) ) return EXTHW_MODE_COM;
  if( strcasecmp( mode, "none" ) )
    ui_error( UI_ERROR_WARNING, "exthw: unknown mode '%s', using 'none'",
              mode );
  return EXTHW_MODE_NONE;
}

int
exthw_mute_internal_ay( void )
{
  return settings_current.exthw_mute_internal &&
         exthw_parse_mode( settings_current.exthw_mode ) != EXTHW_MODE_NONE;
}

/* ---- UDP ---- */

static int
exthw_udp_open( void )
{
  WSADATA wsa_data;
  struct addrinfo hints, *res = NULL;
  char port[ 16 ];
  const char *host = settings_current.exthw_udp_host;
  int error;

  if( !host || !*host ) host = "127.0.0.1";

  error = WSAStartup( MAKEWORD( 2, 2 ), &wsa_data );
  if( error ) {
    ui_error( UI_ERROR_ERROR, "exthw: WSAStartup failed (%d)", error );
    return 1;
  }
  wsa_started = 1;

  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  snprintf( port, sizeof( port ), "%d", settings_current.exthw_udp_port );

  error = getaddrinfo( host, port, &hints, &res );
  if( error || !res ) {
    ui_error( UI_ERROR_ERROR, "exthw: cannot resolve UDP target %s:%s (%d)",
              host, port, error );
    return 1;
  }

  udp_socket = socket( res->ai_family, SOCK_DGRAM, IPPROTO_UDP );
  if( udp_socket == INVALID_SOCKET ) {
    ui_error( UI_ERROR_ERROR, "exthw: cannot open UDP socket (%d)",
              WSAGetLastError() );
    freeaddrinfo( res );
    return 1;
  }

  memcpy( &udp_addr, res->ai_addr, res->ai_addrlen );
  udp_addr_len = (int)res->ai_addrlen;
  freeaddrinfo( res );

  /* Nonblocking: a full send buffer drops packets instead of stalling */
  {
    u_long nonblocking = 1;
    ioctlsocket( udp_socket, FIONBIO, &nonblocking );
  }

  return 0;
}

static void
exthw_udp_close( void )
{
  if( udp_socket != INVALID_SOCKET ) {
    closesocket( udp_socket );
    udp_socket = INVALID_SOCKET;
  }
  if( wsa_started ) {
    WSACleanup();
    wsa_started = 0;
  }
}

static void
exthw_udp_send( unsigned char areg, unsigned char adata )
{
  char msg[ 2 ];

  if( udp_socket == INVALID_SOCKET ) return;
  msg[0] = areg;
  msg[1] = adata;
  sendto( udp_socket, msg, sizeof( msg ), 0, (struct sockaddr *)&udp_addr,
          udp_addr_len );
}

/* ---- COM ---- */

static int
exthw_com_open( void )
{
  char name[ 64 ];
  DCB dcb;
  COMMTIMEOUTS timeouts;
  const char *port = settings_current.exthw_com_port;

  if( !port || !*port ) {
    ui_error( UI_ERROR_ERROR, "exthw: COM port not set (exthwcomport)" );
    return 1;
  }

  /* "\\.\" prefix is required for COM10 and above */
  if( !strncmp( port, "\\\\.\\", 4 ) )
    snprintf( name, sizeof( name ), "%s", port );
  else
    snprintf( name, sizeof( name ), "\\\\.\\%s", port );

  com_handle = CreateFileA( name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, 0, NULL );
  if( com_handle == INVALID_HANDLE_VALUE ) {
    ui_error( UI_ERROR_ERROR, "exthw: cannot open %s (error %lu)", port,
              (unsigned long)GetLastError() );
    return 1;
  }

  memset( &dcb, 0, sizeof( dcb ) );
  dcb.DCBlength = sizeof( dcb );
  if( !GetCommState( com_handle, &dcb ) ) {
    ui_error( UI_ERROR_ERROR, "exthw: GetCommState on %s failed (%lu)", port,
              (unsigned long)GetLastError() );
    return 1;
  }

  dcb.BaudRate = settings_current.exthw_com_baud;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fNull = FALSE;
  dcb.fAbortOnError = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;

  if( !SetCommState( com_handle, &dcb ) ) {
    ui_error( UI_ERROR_ERROR, "exthw: cannot set %s to %d Bd (error %lu)",
              port, settings_current.exthw_com_baud,
              (unsigned long)GetLastError() );
    return 1;
  }

  /* Short write timeout so an unplugged Arduino cannot freeze emulation */
  memset( &timeouts, 0, sizeof( timeouts ) );
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.WriteTotalTimeoutConstant = 50;
  SetCommTimeouts( com_handle, &timeouts );

  PurgeComm( com_handle, PURGE_TXCLEAR | PURGE_RXCLEAR );

  return 0;
}

static void
exthw_com_close( void )
{
  if( com_handle != INVALID_HANDLE_VALUE ) {
    CloseHandle( com_handle );
    com_handle = INVALID_HANDLE_VALUE;
  }
}

static void
exthw_com_send( unsigned char cmd, unsigned char areg, unsigned char adata )
{
  unsigned char msg[ 3 ];
  DWORD written;

  if( com_handle == INVALID_HANDLE_VALUE ) return;
  msg[0] = cmd;
  msg[1] = areg;
  msg[2] = adata;
  WriteFile( com_handle, msg, sizeof( msg ), &written, NULL );
}

/* Send a raw block: one UDP datagram or one COM write */
static void
exthw_send_block( const unsigned char *data, size_t len )
{
  DWORD written;

  switch( exthw_mode ) {
  case EXTHW_MODE_UDP:
    if( udp_socket != INVALID_SOCKET )
      sendto( udp_socket, (const char *)data, (int)len, 0,
              (struct sockaddr *)&udp_addr, udp_addr_len );
    break;
  case EXTHW_MODE_COM:
    if( com_handle != INVALID_HANDLE_VALUE )
      WriteFile( com_handle, data, (DWORD)len, &written, NULL );
    break;
  default:
    break;
  }
}

/* ---- timed mode ---- */

static int
exthw_latency_ms( void )
{
  const char *s = settings_current.exthw_latency;
  int ms = s ? atoi( s ) : 40;

  if( ms < 10 ) ms = 10;
  if( ms > 250 ) ms = 250;
  return ms;
}

static void
frame_put( unsigned char b )
{
  if( frame_len == frame_alloc ) {
    frame_alloc = frame_alloc ? frame_alloc * 2 : 1024;
    frame_buf = libspectrum_realloc( frame_buf, frame_alloc );
  }
  frame_buf[ frame_len++ ] = b;
}

/* Short entry forms (same reg as the previous entry of this frame, value
   < 32): SHORT_DELTA|val + int8 dt delta, or SHORT_SAME|val for equal dt */
#define EXTHW_SHORT_DELTA 0x40
#define EXTHW_SHORT_SAME 0x60
#define EXTHW_MAX_FRAME 1024   /* firmware ring on Nano is ~1500 bytes */

static int frame_prev_reg = -1;
static libspectrum_dword frame_prev_dt = 0;

static void
exthw_timed_write( unsigned char reg, unsigned char val,
                   libspectrum_dword now )
{
  libspectrum_dword dt = now >= frame_last_t ? now - frame_last_t : 0;
  long delta = (long)dt - (long)frame_prev_dt;

  /* Frame full: skip the write, its time goes to the next one */
  if( frame_len + 8 > EXTHW_MAX_FRAME ) return;

  if( now > frame_last_t ) frame_last_t = now;
  if( reg >= 8 && reg <= 10 ) val &= 0x1f;   /* the chip keeps 5 bits */

  if( reg == frame_prev_reg && val < 32 && delta >= -128 && delta <= 127 ) {
    if( delta == 0 ) {
      frame_put( EXTHW_SHORT_SAME | val );
    } else {
      frame_put( EXTHW_SHORT_DELTA | val );
      frame_put( (unsigned char)( delta & 0xff ) );
    }
  } else {
    libspectrum_dword v = dt;
    frame_put( reg );
    frame_put( val );
    do {
      unsigned char b = v & 0x7f;
      v >>= 7;
      frame_put( v ? b | 0x80 : b );
    } while( v );
  }
  frame_prev_reg = reg;
  frame_prev_dt = dt;
}

static void
exthw_timed_reset_frame( void )
{
  frame_len = 0;
  frame_last_t = 0;
  frame_prev_reg = -1;
  frame_prev_dt = 0;
}

static void
exthw_timed_config( void )
{
  libspectrum_dword cpu_hz = machine_current->timings.processor_speed;
  libspectrum_dword frame_t = machine_current->timings.tstates_per_frame;
  int latency = exthw_latency_ms();
  unsigned char msg[ 10 ];

  if( cpu_hz == sent_cpu_hz && frame_t == sent_frame_t &&
      latency == sent_latency )
    return;

  msg[0] = EXTHW_TIMED_CONFIG;
  msg[1] = cpu_hz & 0xff; msg[2] = ( cpu_hz >> 8 ) & 0xff;
  msg[3] = ( cpu_hz >> 16 ) & 0xff; msg[4] = ( cpu_hz >> 24 ) & 0xff;
  msg[5] = frame_t & 0xff; msg[6] = ( frame_t >> 8 ) & 0xff;
  msg[7] = ( frame_t >> 16 ) & 0xff; msg[8] = ( frame_t >> 24 ) & 0xff;
  msg[9] = (unsigned char)latency;
  exthw_send_block( msg, sizeof( msg ) );

  sent_cpu_hz = cpu_hz;
  sent_frame_t = frame_t;
  sent_latency = latency;
}

static void
exthw_timed_flush_mute( void )
{
  unsigned char msg[ 3 ] = { EXTHW_TIMED_FLUSH, 0, 0 };

  exthw_send_block( msg, sizeof( msg ) );
  exthw_timed_reset_frame();
}

void
exthw_module_frame_end( libspectrum_dword frame_length GCC_UNUSED )
{
  unsigned char *pkt;
  unsigned char x = 0;
  size_t i, len;

  if( !timed || !machine_current ) return;

  exthw_timed_config();

  /* Clamp to u16; a frame never gets near that in practice */
  len = frame_len > 0xffff ? 0xffff : frame_len;
  pkt = libspectrum_new( unsigned char, len + 5 );
  pkt[0] = EXTHW_TIMED_FRAME;
  pkt[1] = frame_seq++;
  pkt[2] = len & 0xff;
  pkt[3] = ( len >> 8 ) & 0xff;
  for( i = 0; i < len; i++ ) {
    pkt[ 4 + i ] = frame_buf[i];
    x ^= frame_buf[i];
  }
  pkt[ 4 + len ] = x;
  exthw_send_block( pkt, len + 5 );
  libspectrum_free( pkt );

  exthw_timed_reset_frame();
}

/* ---- public ---- */

void
exthw_module_senday_t( char areg, char adata, libspectrum_dword now )
{
  unsigned char reg = (unsigned char)areg;

  if( !timed ) {
    exthw_module_senday( areg, adata );
    return;
  }
  if( reg < 14 ) exthw_timed_write( reg, (unsigned char)adata, now );
}

void
exthw_module_senday( char areg, char adata )
{
  unsigned char reg = (unsigned char)areg;

  /* R14/R15 are the AY I/O port (RS232/MIDI/keypad on 128K), not sound */
  if( reg == 14 || reg == 15 ) return;

  switch( exthw_mode ) {
  case EXTHW_MODE_UDP:
    exthw_udp_send( reg, (unsigned char)adata );
    break;
  case EXTHW_MODE_COM:
    /* Arduino knows only AY registers; log/cmd pseudo registers are UDP-only */
    if( reg < 14 ) exthw_com_send( EXTHW_COM_WRITE, reg, (unsigned char)adata );
    break;
  default:
    break;
  }
}

void
exthw_module_sendlog( char adata )
{
  exthw_module_senday( (char)EXTHW_REG_LOG, adata );
}

void
exthw_module_sendcmd( char acmd )
{
  exthw_module_senday( (char)EXTHW_REG_CMD, acmd );
}

void
exthwmod_aymute( void )
{
  int i;

  if( timed ) {
    exthw_timed_flush_mute();
    return;
  }
  for( i = 8; i <= 10; i++ ) exthw_module_senday( i, 0 );
}

/* Store the mode as the options dialog spells it ("None", "UDP", "COM") */
static void
exthw_canonical_mode( exthw_mode_t mode )
{
  static const char * const names[] = { "None", "UDP", "COM" };

  libspectrum_free( settings_current.exthw_mode );
  settings_current.exthw_mode = utils_safe_strdup( names[ mode ] );
}

static int
exthwmod_init( void *context )
{
  exthw_mode_t mode = exthw_parse_mode( settings_current.exthw_mode );
  int error = 0;

  exthw_canonical_mode( mode );

  switch( mode ) {
  case EXTHW_MODE_UDP: error = exthw_udp_open(); break;
  case EXTHW_MODE_COM: error = exthw_com_open(); break;
  default: break;
  }

  if( error ) {
    /* Keep emulating without external output */
    exthw_udp_close();
    exthw_com_close();
    exthw_mode = EXTHW_MODE_NONE;
    return 0;
  }

  exthw_mode = mode;
  timed = exthw_mode != EXTHW_MODE_NONE && settings_current.exthw_timed;
  sent_cpu_hz = sent_frame_t = 0;
  sent_latency = -1;
  exthw_timed_reset_frame();
  if( exthw_mode == EXTHW_MODE_COM )
    exthw_com_send( EXTHW_COM_RESET, 0, 0 );
  exthwmod_aymute();
  exthw_module_sendlog( 1 );
  return 0;
}

static void
exthwmod_end( void )
{
  exthwmod_aymute();
  exthw_module_sendlog( (char)255 );
  timed = 0;
  exthw_mode = EXTHW_MODE_NONE;
  exthw_udp_close();
  exthw_com_close();
}

void
exthw_options_posthook( void )
{
  exthwmod_end();
  exthwmod_init( NULL );
}

void
exthw_register_startup( void )
{
  startup_manager_module dependencies[] = {
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_EXTHW, dependencies,
                            ARRAY_SIZE( dependencies ), exthwmod_init, NULL,
                            exthwmod_end );
}
