/* exthwmod.h: external hw module (real AY chip output via UDP or COM)
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

/* Settings (fuse.cfg / command line), see exthw/README.md:
     exthw_mode          "none" | "udp" | "com"
     exthw_udp_host      e.g. "127.0.0.1"
     exthw_udp_port      e.g. 9101
     exthw_com_port      e.g. "COM3"
     exthw_com_baud      e.g. 2000000
     exthw_mute_internal 1 = silence Fuse's own AY emulation when mode != none */

void exthw_register_startup( void );

void exthw_module_senday( char areg, char adata );
void exthw_module_sendlog( char adata );
void exthwmod_aymute( void );

/* Reopen the output after the options dialog changed the settings */
void exthw_options_posthook( void );

/* Nonzero when the internal AY emulation should be silenced */
int exthw_mute_internal_ay( void );

#endif				/* #ifndef FUSE_EXTHWMOD_H */
