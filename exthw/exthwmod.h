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

#ifndef FUSE_EXTHWMOD_H
#define FUSE_EXTHWMOD_H

#ifdef WIN32
#include <winsock2.h>
#endif

void exthw_register_startup( void );

/* Socket handling */

#ifndef WIN32
typedef int exhw_socket_t;
#else                           /* #ifndef WIN32 */
typedef SOCKET exhw_socket_t;
#endif


extern const exhw_socket_t ay_socket_invalid;
extern const int ay_socket_EBADF;

//int ay_socket_close( exhw_socket_t fd );
//int ay_socket_get_error( void );
//const char *ay_socket_get_strerror( void );

typedef struct exhw_module_t exhw_module_t;

//static exhw_module_t *exthwmod = NULL;
//exhw_module_t* exthw_module_alloc( void );
//void exthw_module_free( exhw_module_t *self );
void exthw_module_senday(char areg, char adata);
void exthw_module_sendlog(char adata);
void exthwmod_aymute(void);
//exhw_socket_t exthw_module_get_read_fd( exhw_module_t *self );
//void exthw_module_wake( exhw_module_t *self );
//void exthw_module_discard_data( exhw_module_t *self );

#endif				/* #ifndef FUSE_EXTHWMOD_H */
