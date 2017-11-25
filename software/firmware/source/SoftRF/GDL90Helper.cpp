/*
 * GDL90Helper.cpp
 * Copyright (C) 2016-2017 Linar Yusupov
 *
 * Inspired by Eric's Dey Python GDL-90 encoder:
 * https://github.com/etdey/gdl90
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <TimeLib.h>
#include <WiFiUdp.h>
#include <lib_crc.h>

#include "GDL90Helper.h"
#include "GNSSHelper.h"
#include "SoftRF.h"
#include "WiFiHelper.h"

#define isValidFix() (gnss.location.isValid() && (gnss.location.age() <= 3000))

static GDL90_Msg_HeartBeat_t HeartBeat;
static GGDL90_Msg_Traffic_t Traffic;

extern ufo_t fo, Container[MAX_TRACKING_OBJECTS];
extern ufo_t ThisAircraft;
extern char UDPpacketBuffer[256];

/* convert a signed latitude to 2s complement ready for 24-bit packing */
uint32_t makeLatitude(float latitude)
{
    int32_t int_lat;

    if (latitude > 90.0) {
      latitude = 90.0;
    }

    if (latitude < -90.0) {
      latitude = -90.0;
    }

    int_lat = (int) (latitude * (0x800000 / 180.0));

    if (int_lat < 0) {
      int_lat = (0x1000000 + int_lat) & 0xffffff;  /* 2s complement */  
    }

    return(int_lat);    
}

/* convert a signed longitude to 2s complement ready for 24-bit packing */
uint32_t makeLongitude(float longitude)
{
    int32_t int_lon;

    if (longitude > 180.0) {
      longitude = 180.0;
    }

    if (longitude < -180.0) {
      longitude = -180.0;
    }

    int_lon = (int) (longitude * (0x800000 / 180.0));

    if (int_lon < 0) {
      int_lon = (0x1000000 + int_lon) & 0xffffff;  /* 2s complement */  
    }

    return(int_lon);    
}


uint32_t pack24bit(uint32_t num)
{
  return( ((num & 0xff0000) >> 16) | (num & 0x00ff00) | ((num & 0xff) << 16) );
}

uint16_t calcFCS(uint8_t msg_id, uint8_t *msg, int size)
{
  uint16_t crc16 = 0x0000;  /* seed value */

  crc16 = update_crc_gdl90(crc16, msg_id);

  for (int i=0; i < size; i++)
  {
    crc16 = update_crc_gdl90(crc16, msg[i]);
  }    

  return(crc16);
}

uint8_t *EscapeFilter(uint8_t *buf, uint8_t *p, int size)
{
  while (size--) {
    if (*p != 0x7D && *p != 0x7E) {
      *buf++ = *p++;
    } else {
      *buf++ = 0x7D;
      *buf++ = *p++ ^ 0x20;
    }   
  }

  return (buf);
}

void *msgHeartbeat()
{
  time_t ts = elapsedSecsToday(now());

  /* Status Byte 1 */
  HeartBeat.gnss_pos_valid  = 1 /* isValidFix() */ ;
  HeartBeat.maint_reqd      = 0;
  HeartBeat.ident           = 0;
  HeartBeat.addr_type       = 0;
  HeartBeat.gnss_bat_low    = 0;
  HeartBeat.ratcs           = 0;
//HeartBeat.reserved1       = 0;
  HeartBeat.uat_init        = 1;

  /* Status Byte 2 */
  HeartBeat.time_stamp_ms   = (ts >> 16) & 1;
  HeartBeat.csa_req         = 0;
  HeartBeat.csa_not_avail   = 0;
//HeartBeat.reserved2       = 0;
//HeartBeat.reserved3       = 0;
//HeartBeat.reserved4       = 0;
//HeartBeat.reserved5       = 0;
  HeartBeat.utc_ok          = 0;

  HeartBeat.time_stamp      = (ts & 0xFFFF);   // LSB first
  HeartBeat.message_counts  = 0;

  return (&HeartBeat);
}

void *msgType10and20(ufo_t *aircraft)
{
  int altitude = ((int)(aircraft->altitude) + 1000) / 25;
  int trackHeading = (int)(aircraft->course / (360.0 / 256)); /* convert to 1.4 deg single byte */

  if (altitude < 0) {
    altitude = 0;  
  }
  if (altitude > 0xffe) {
    altitude = 0xffe;  
  }
 
  uint8_t misc = 9;
  //altitude = 0x678;
  
  uint16_t vert_vel = 0 /* 0xdef */;
  uint16_t horiz_vel = 0 /* 0x123 */;

  Traffic.alert_status  = 0 /* 0x1 */;
  Traffic.addr_type     = 0 /* 0x2 */;
  Traffic.addr          = pack24bit(aircraft->addr) /* pack24bit(0x345678) */;
  Traffic.latitude      = pack24bit(makeLatitude(aircraft->latitude)) /* pack24bit(0x9abcde) */;
  Traffic.longitude     = pack24bit(makeLongitude(aircraft->longitude)) /* pack24bit(0xf12345) */;

  /*
   * workaround against "implementation dependant"
   * XTENSA's GCC bitmap layout in structures
   */
  Traffic.altitude      = ((altitude >> 4) & 0xFF) | (misc << 8);
  Traffic.misc          = (altitude & 0x00F);

  Traffic.nic           = 8 /* 0xa */;
  Traffic.nacp          = 8 /* 0xb */;

  /*
   * workaround against "implementation dependant"
   * XTENSA's GCC bitmap layout in structures
   */
  Traffic.vert_vel      = ((vert_vel >> 4) & 0x0FF) | (horiz_vel & 0xF00);
  Traffic.horiz_vel     = ((horiz_vel & 0xFF) << 4) | (vert_vel & 0x00F);

  Traffic.track         = (trackHeading & 0xFF) /* 0x03 */;
  Traffic.emit_cat      = 1 /* 0x4 */;
  strcpy((char *)Traffic.callsign, "FLARM");
  Traffic.emerg_code    = 0 /* 0x5 */;
//Traffic.reserved      = 0;

  return (&Traffic);
}

size_t makeHeartbeat(uint8_t *buf)
{
  uint8_t *ptr = buf;
  uint8_t *msg = (uint8_t *) msgHeartbeat();
  uint16_t fcs = calcFCS(GDL90_HEARTBEAT_MSG_ID, msg, sizeof(GDL90_Msg_HeartBeat_t));
  uint8_t fcs_lsb, fcs_msb;
  
  fcs_lsb = fcs        & 0xFF;
  fcs_msb = (fcs >> 8) & 0xFF;

  *ptr++ = 0x7E; /* Start flag */
  *ptr++ = GDL90_HEARTBEAT_MSG_ID;
  ptr = EscapeFilter(ptr, msg, sizeof(GDL90_Msg_HeartBeat_t)); 
  ptr = EscapeFilter(ptr, &fcs_lsb, 1); 
  ptr = EscapeFilter(ptr, &fcs_msb, 1);  
  *ptr++ = 0x7E; /* Stop flag */

  return(ptr-buf);
}

size_t makeType10and20(uint8_t *buf, uint8_t id, ufo_t *aircraft)
{
  uint8_t *ptr = buf;
  uint8_t *msg = (uint8_t *) msgType10and20(aircraft);
  uint16_t fcs = calcFCS(id, msg, sizeof(GGDL90_Msg_Traffic_t));
  uint8_t fcs_lsb, fcs_msb;
  
  fcs_lsb = fcs        & 0xFF;
  fcs_msb = (fcs >> 8) & 0xFF;

  *ptr++ = 0x7E; /* Start flag */
  *ptr++ = id;
  ptr = EscapeFilter(ptr, msg, sizeof(GGDL90_Msg_Traffic_t)); 
  ptr = EscapeFilter(ptr, &fcs_lsb, 1); 
  ptr = EscapeFilter(ptr, &fcs_msb, 1);  
  *ptr++ = 0x7E; /* Stop flag */

  return(ptr-buf);
}

#define makeOwnershipReport(b,a)  makeType10and20(b, GDL90_OWNSHIP_MSG_ID, a)
#define makeTrafficReport(b,a)    makeType10and20(b, GDL90_TRAFFIC_MSG_ID, a)

void GDL90_Export()
{
  size_t size;
  float distance;
  time_t this_moment = now();
  uint8_t *buf = (uint8_t *) UDPpacketBuffer;
  IPAddress broadcastIP = WiFi_get_broadcast();

  Uni_Udp.beginPacket(broadcastIP, GDL90_DST_PORT);
  size = makeHeartbeat(buf);
  Uni_Udp.write(buf, size);
  Uni_Udp.endPacket();

  Uni_Udp.beginPacket(broadcastIP, GDL90_DST_PORT);
  size = makeOwnershipReport(buf, &ThisAircraft);
  Uni_Udp.write(buf, size);
  Uni_Udp.endPacket();

  for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].addr && (this_moment - Container[i].timestamp) <= EXPORT_EXPIRATION_TIME) {

      distance = gnss.distanceBetween(ThisAircraft.latitude, ThisAircraft.longitude, Container[i].latitude, Container[i].longitude);

      if (distance < EXPORT_DISTANCE_FAR) {
        Uni_Udp.beginPacket(broadcastIP, GDL90_DST_PORT);
        size = makeTrafficReport(buf, &Container[i]);
        Uni_Udp.write(buf, size);
        Uni_Udp.endPacket();
      }
    }
  }
}
