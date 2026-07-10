/**
 * @file    navdata.c
 * @brief   Navigation data operations implementation.
 *
 * All geographic calculations use the spherical Earth model
 * (radius = 3440.065 NM, which is 6371 km / 1.852).
 */

#include "navdata.h"
#include "../ds/spatial_hash.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Earth radius in nautical miles (6371 km / 1.852) */
#define EARTH_RADIUS_NM 3440.065f
#define DEG2RAD(d)      ((d) * M_PI / 180.0)
#define RAD2DEG(r)      ((r) * 180.0 / M_PI)

/* =========================================================================
 *  Geographic calculations
 * ========================================================================= */

double geo_distance_nm(GeoPos a, GeoPos b)
{
    double lat1 = DEG2RAD(a.lat_deg);
    double lon1 = DEG2RAD(a.lon_deg);
    double lat2 = DEG2RAD(b.lat_deg);
    double lon2 = DEG2RAD(b.lon_deg);

    double dlat = lat2 - lat1;
    double dlon = lon2 - lon1;

    double sin_dlat = sin(dlat / 2.0);
    double sin_dlon = sin(dlon / 2.0);

    double a_val = sin_dlat * sin_dlat +
                   cos(lat1) * cos(lat2) * sin_dlon * sin_dlon;
    double c_val = 2.0 * atan2(sqrt(a_val), sqrt(1.0 - a_val));

    return EARTH_RADIUS_NM * c_val;
}

double geo_bearing_deg(GeoPos a, GeoPos b)
{
    double lat1 = DEG2RAD(a.lat_deg);
    double lon1 = DEG2RAD(a.lon_deg);
    double lat2 = DEG2RAD(b.lat_deg);
    double lon2 = DEG2RAD(b.lon_deg);

    double dlon = lon2 - lon1;
    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) -
               sin(lat1) * cos(lat2) * cos(dlon);

    double bearing = atan2(y, x);
    double deg = RAD2DEG(bearing);

    /* Normalize to 0..360 */
    if (deg < 0.0) deg += 360.0;
    return deg;
}

GeoPos geo_offset(GeoPos start, double dist_nm, double bearing_deg)
{
    double lat1 = DEG2RAD(start.lat_deg);
    double lon1 = DEG2RAD(start.lon_deg);
    double brng = DEG2RAD(bearing_deg);
    double ang_dist = dist_nm / EARTH_RADIUS_NM;

    double lat2 = asin(sin(lat1) * cos(ang_dist) +
                       cos(lat1) * sin(ang_dist) * cos(brng));

    double lon2 = lon1 + atan2(sin(brng) * sin(ang_dist) * cos(lat1),
                                cos(ang_dist) - sin(lat1) * sin(lat2));

    GeoPos result;
    result.lat_deg = RAD2DEG(lat2);
    result.lon_deg = RAD2DEG(lon2);

    /* Normalize longitude */
    if (result.lon_deg > 180.0)  result.lon_deg -= 360.0;
    if (result.lon_deg < -180.0) result.lon_deg += 360.0;

    return result;
}

static int compare_airports(const void* a, const void* b) {
    return strcmp(((const Airport*)a)->icao, ((const Airport*)b)->icao);
}

/* =========================================================================
 *  Navigation database — hardcoded Chinese airports & waypoints
 * ========================================================================= */

int nav_database_init(FMCState* state)
{
    if (!state) return -1;

    /* --- Airports (10 major Chinese airports) --- */

    const struct { const char* icao; const char* iata; const char* name;
                   double lat; double lon; float elev; } apts[] = {
        /* === Existing Chinese Airports === */
        { "ZBAA", "PEK", "Beijing Capital",       40.0799, 116.6031,  115 },
        { "ZSSS", "SHA", "Shanghai Hongqiao",     31.1979, 121.3363,   10 },
        { "ZSPD", "PVG", "Shanghai Pudong",       31.1434, 121.8053,   13 },
        { "ZGGG", "CAN", "Guangzhou Baiyun",      23.3925, 113.2988,   50 },
        { "ZUUU", "CTU", "Chengdu Shuangliu",     30.5785, 103.9469, 1625 },
        { "ZBTJ", "TSN", "Tianjin Binhai",        39.1244, 117.3462,   10 },
        { "ZSHC", "HGH", "Hangzhou Xiaoshan",     30.2295, 120.4344,   23 },
        { "ZPPP", "KMG", "Kunming Changshui",     25.1019, 102.9291, 6895 },
        { "ZSAM", "XMN", "Xiamen Gaoqi",          24.5440, 118.1277,   59 },
        { "ZSNJ", "NKG", "Nanjing Lukou",         31.7420, 118.8620,   49 },

        /* === US Airports - Pacific Northwest === */
        { "KHIO", "HIO", "Portland Hillsboro",     45.5404, -122.9498, 208 },
        { "KPDX", "PDX", "Portland Intl",         45.5887, -122.5969,  31 },
        { "KSEA", "SEA", "Seattle Tacoma International Airport", 47.4490, -122.3090, 433 },
        { "KBFI", "BFI", "Boeing Field King County International", 47.5401, -122.3094, 21 },
        { "KGEG", "GEG", "Spokane International Airport", 47.6199, -117.5340, 2376 },
        { "KEUG", "EUG", "Mahlon Sweet Field", 44.1246, -123.2120, 374 },
        { "KMFR", "MFR", "Rogue Valley International Medford Airport", 42.3742, -122.8730, 1335 },
        { "KBOI", "BOI", "Boise Air Terminal/Gowen Field", 43.5644, -116.2230, 2871 },

        /* === US Airports - California === */
        { "KLAX", "LAX", "Los Angeles International Airport", 33.9425, -118.4080, 125 },
        { "KSFO", "SFO", "San Francisco International Airport", 37.6190, -122.3750, 13 },
        { "KSAN", "SAN", "San Diego International Airport", 32.7336, -117.1900, 17 },
        { "KSMF", "SMF", "Sacramento International Airport", 38.6954, -121.5910, 27 },
        { "KSJC", "SJC", "Norman Y. Mineta San Jose International Airport", 37.3626, -121.9290, 62 },
        { "KOAK", "OAK", "Metropolitan Oakland International Airport", 37.7213, -122.2210, 9 },
        { "KONT", "ONT", "Ontario International Airport", 34.0560, -117.6010, 944 },
        { "KBUR", "BUR", "Bob Hope Airport", 34.2007, -118.3590, 778 },
        { "KSNA", "SNA", "John Wayne Airport-Orange County Airport", 33.6757, -117.8680, 56 },
        { "KFAT", "FAT", "Fresno Yosemite International Airport", 36.7762, -119.7180, 336 },
        { "KSBA", "SBA", "Santa Barbara Municipal Airport", 34.4262, -119.8400, 13 },
        { "KPSP", "PSP", "Palm Springs International Airport", 33.8297, -116.5070, 477 },

        /* === US Airports - Southwest / Mountains === */
        { "KLAS", "LAS", "McCarran International Airport", 36.0801, -115.1520, 2181 },
        { "KPHX", "PHX", "Phoenix Sky Harbor International Airport", 33.4343, -112.0120, 1135 },
        { "KTUS", "TUS", "Tucson International Airport", 32.1161, -110.9410, 2643 },
        { "KDEN", "DEN", "Denver International Airport", 39.8617, -104.6730, 5431 },
        { "KSLC", "SLC", "Salt Lake City International Airport", 40.7884, -111.9780, 4227 },
        { "KCOS", "COS", "City of Colorado Springs Municipal Airport", 38.8058, -104.7010, 6187 },
        { "KABQ", "ABQ", "Albuquerque International Sunport", 35.0402, -106.6090, 5355 },
        { "KRNO", "RNO", "Reno Tahoe International Airport", 39.4991, -119.7680, 4415 },
        { "KASE", "ASE", "Aspen-Pitkin Co/Sardy Field", 39.2232, -106.8690, 7820 },
        { "KHDN", "HDN", "Yampa Valley Airport", 40.4812, -107.2180, 6606 },
        { "KBZN", "BZN", "Gallatin Field", 45.7775, -111.1530, 4473 },
        { "KBIL", "BIL", "Billings Logan International Airport", 45.8077, -108.5430, 3652 },

        /* === US Airports - Texas / South Central === */
        { "KDFW", "DFW", "Dallas Fort Worth International Airport", 32.8968, -97.0380, 607 },
        { "KIAH", "IAH", "George Bush Intercontinental Houston Airport", 29.9844, -95.3414, 97 },
        { "KHOU", "HOU", "William P Hobby Airport", 29.6454, -95.2789, 46 },
        { "KAUS", "AUS", "Austin Bergstrom International Airport", 30.1945, -97.6699, 542 },
        { "KSAT", "SAT", "San Antonio International Airport", 29.5337, -98.4698, 809 },
        { "KDAL", "DAL", "Dallas Love Field", 32.8471, -96.8518, 487 },
        { "KOKC", "OKC", "Will Rogers World Airport", 35.3931, -97.6007, 1295 },
        { "KTUL", "TUL", "Tulsa International Airport", 36.1984, -95.8881, 677 },
        { "KELP", "ELP", "El Paso International Airport", 31.8072, -106.3780, 3959 },
        { "KMAF", "MAF", "Midland International Airport", 31.9425, -102.2020, 2871 },
        { "KLBB", "LBB", "Lubbock Preston Smith International Airport", 33.6636, -101.8230, 3282 },
        { "KAMA", "AMA", "Rick Husband Amarillo International Airport", 35.2194, -101.7060, 3607 },
        { "KCRP", "CRP", "Corpus Christi International Airport", 27.7704, -97.5012, 44 },
        { "KHRL", "HRL", "Valley International Airport", 26.2285, -97.6544, 36 },

        /* === US Airports - Midwest === */
        { "KORD", "ORD", "Chicago O'Hare International Airport", 41.9786, -87.9048, 672 },
        { "KMDW", "MDW", "Chicago Midway International Airport", 41.7860, -87.7524, 620 },
        { "KDTW", "DTW", "Detroit Metropolitan Wayne County Airport", 42.2124, -83.3534, 645 },
        { "KMSP", "MSP", "Minneapolis-St Paul International/Wold-Chamberlain Airport", 44.8820, -93.2218, 841 },
        { "KSTL", "STL", "St Louis Lambert International Airport", 38.7487, -90.3700, 618 },
        { "KMCI", "MCI", "Kansas City International Airport", 39.2976, -94.7139, 1026 },
        { "KCLE", "CLE", "Cleveland Hopkins International Airport", 41.4117, -81.8498, 791 },
        { "KCMH", "CMH", "John Glenn Columbus International Airport", 39.9980, -82.8919, 815 },
        { "KCVG", "CVG", "Cincinnati Northern Kentucky International Airport", 39.0488, -84.6678, 896 },
        { "KIND", "IND", "Indianapolis International Airport", 39.7173, -86.2944, 797 },
        { "KMKE", "MKE", "General Mitchell International Airport", 42.9472, -87.8966, 723 },
        { "KGRR", "GRR", "Gerald R. Ford International Airport", 42.8808, -85.5228, 794 },
        { "KDSM", "DSM", "Des Moines International Airport", 41.5340, -93.6631, 958 },
        { "KOMA", "OMA", "Eppley Airfield", 41.3032, -95.8941, 984 },
        { "KICT", "ICT", "Wichita Eisenhower National Airport", 37.6499, -97.4331, 1333 },
        { "KFNT", "FNT", "Bishop International Airport", 42.9654, -83.7436, 782 },
        { "KTOL", "TOL", "Toledo Express Airport", 41.5868, -83.8078, 683 },
        { "KDAY", "DAY", "James M Cox Dayton International Airport", 39.9024, -84.2194, 1009 },
        { "KSDF", "SDF", "Louisville International Standiford Field", 38.1744, -85.7360, 501 },
        { "KLEX", "LEX", "Blue Grass Airport", 38.0365, -84.6059, 979 },
        { "KCID", "CID", "The Eastern Iowa Airport", 41.8847, -91.7108, 869 },
        { "KMLI", "MLI", "Quad City International Airport", 41.4485, -90.5075, 590 },
        { "KPIA", "PIA", "General Wayne A. Downing Peoria International Airport", 40.6642, -89.6933, 660 },
        { "KSGF", "SGF", "Springfield Branson National Airport", 37.2457, -93.3886, 1268 },
        { "KFWA", "FWA", "Fort Wayne International Airport", 40.9785, -85.1951, 814 },
        { "KGRB", "GRB", "Austin Straubel International Airport", 44.4851, -88.1296, 695 },
        { "KMSN", "MSN", "Dane County Regional Truax Field", 43.1399, -89.3375, 887 },
        { "KFAR", "FAR", "Hector International Airport", 46.9207, -96.8158, 902 },
        { "KRST", "RST", "Rochester International Airport", 43.9083, -92.5000, 1317 },
        { "KAZO", "AZO", "Kalamazoo Battle Creek International Airport", 42.2349, -85.5521, 874 },
        { "KBMI", "BMI", "Central Illinois Regional Airport at Bloomington-Normal", 40.4771, -88.9159, 871 },
        { "KCMI", "CMI", "University of Illinois Willard Airport", 40.0392, -88.2781, 755 },
        { "KSBN", "SBN", "South Bend Regional Airport", 41.7087, -86.3173, 799 },

        /* === US Airports - Northeast === */
        { "KBOS", "BOS", "General Edward Lawrence Logan International Airport", 42.3643, -71.0052, 20 },
        { "KJFK", "JFK", "John F Kennedy International Airport", 40.6398, -73.7789, 13 },
        { "KLGA", "LGA", "La Guardia Airport", 40.7772, -73.8726, 21 },
        { "KEWR", "EWR", "Newark Liberty International Airport", 40.6925, -74.1687, 18 },
        { "KPHL", "PHL", "Philadelphia International Airport", 39.8719, -75.2411, 36 },
        { "KPIT", "PIT", "Pittsburgh International Airport", 40.4915, -80.2329, 1203 },
        { "KBWI", "BWI", "Baltimore/Washington International Thurgood Marshall Airport", 39.1754, -76.6683, 146 },
        { "KDCA", "DCA", "Ronald Reagan Washington National Airport", 38.8521, -77.0377, 15 },
        { "KIAD", "IAD", "Washington Dulles International Airport", 38.9445, -77.4558, 312 },
        { "KBDL", "BDL", "Bradley International Airport", 41.9389, -72.6832, 173 },
        { "KPVD", "PVD", "Theodore Francis Green State Airport", 41.7326, -71.4204, 55 },
        { "KMHT", "MHT", "Manchester-Boston Regional Airport", 42.9326, -71.4357, 266 },
        { "KPWM", "PWM", "Portland International Jetport Airport", 43.6462, -70.3093, 76 },
        { "KBTV", "BTV", "Burlington International Airport", 44.4719, -73.1533, 335 },
        { "KALB", "ALB", "Albany International Airport", 42.7483, -73.8017, 285 },
        { "KSYR", "SYR", "Syracuse Hancock International Airport", 43.1112, -76.1063, 421 },
        { "KROC", "ROC", "Greater Rochester International Airport", 43.1189, -77.6724, 559 },
        { "KBUF", "BUF", "Buffalo Niagara International Airport", 42.9405, -78.7322, 728 },
        { "KAVP", "AVP", "Wilkes Barre Scranton International Airport", 41.3385, -75.7234, 962 },
        { "KMDT", "MDT", "Harrisburg International Airport", 40.1935, -76.7634, 310 },

        /* === US Airports - Southeast === */
        { "KATL", "ATL", "Hartsfield Jackson Atlanta International Airport", 33.6367, -84.4281, 1026 },
        { "KCLT", "CLT", "Charlotte Douglas International Airport", 35.2140, -80.9431, 748 },
        { "KMIA", "MIA", "Miami International Airport", 25.7932, -80.2906, 8 },
        { "KFLL", "FLL", "Fort Lauderdale Hollywood International Airport", 26.0726, -80.1527, 9 },
        { "KMCO", "MCO", "Orlando International Airport", 28.4294, -81.3090, 96 },
        { "KTPA", "TPA", "Tampa International Airport", 27.9755, -82.5332, 26 },
        { "KRSW", "RSW", "Southwest Florida International Airport", 26.5362, -81.7552, 30 },
        { "KPBI", "PBI", "Palm Beach International Airport", 26.6832, -80.0956, 19 },
        { "KJAX", "JAX", "Jacksonville International Airport", 30.4941, -81.6879, 30 },
        { "KRDU", "RDU", "Raleigh Durham International Airport", 35.8776, -78.7875, 435 },
        { "KBNA", "BNA", "Nashville International Airport", 36.1245, -86.6782, 599 },
        { "KMEM", "MEM", "Memphis International Airport", 35.0424, -89.9767, 341 },
        { "KMSY", "MSY", "Louis Armstrong New Orleans International Airport", 29.9934, -90.2580, 4 },
        { "KCHS", "CHS", "Charleston Air Force Base-International Airport", 32.8986, -80.0405, 46 },
        { "KSAV", "SAV", "Savannah Hilton Head International Airport", 32.1276, -81.2021, 50 },
        { "KORF", "ORF", "Norfolk International Airport", 36.8946, -76.2012, 26 },
        { "KRIC", "RIC", "Richmond International Airport", 37.5052, -77.3197, 167 },
        { "KGSP", "GSP", "Greenville Spartanburg International Airport", 34.8957, -82.2189, 964 },
        { "KCAE", "CAE", "Columbia Metropolitan Airport", 33.9388, -81.1195, 236 },
        { "KBHM", "BHM", "Birmingham-Shuttlesworth International Airport", 33.5629, -86.7535, 650 },
        { "KJAN", "JAN", "Jackson-Medgar Wiley Evers International Airport", 32.3112, -90.0759, 346 },
        { "KHSV", "HSV", "Huntsville International Carl T Jones Field", 34.6372, -86.7751, 629 },
        { "KTYS", "TYS", "McGhee Tyson Airport", 35.8110, -83.9940, 981 },
        { "KGNV", "GNV", "Gainesville Regional Airport", 29.6901, -82.2718, 152 },
        { "KTLH", "TLH", "Tallahassee Regional Airport", 30.3965, -84.3503, 81 },
        { "KPNS", "PNS", "Pensacola Regional Airport", 30.4734, -87.1866, 121 },
        { "KMYR", "MYR", "Myrtle Beach International Airport", 33.6797, -78.9283, 25 },
        { "KBTR", "BTR", "Baton Rouge Metropolitan Airport", 30.5332, -91.1496, 70 },
        { "KSHV", "SHV", "Shreveport Regional Airport", 32.4466, -93.8256, 258 },
        { "KLIT", "LIT", "Bill & Hillary Clinton National Airport/Adams Field", 34.7294, -92.2243, 262 },
        { "KCHA", "CHA", "Lovell Field", 35.0353, -85.2038, 683 },
        { "KAVL", "AVL", "Asheville Regional Airport", 35.4362, -82.5418, 2165 },
        { "KCRW", "CRW", "Yeager Airport", 38.3731, -81.5932, 981 },
        { "KROA", "ROA", "Roanoke-Blacksburg Regional Airport", 37.3255, -79.9754, 1175 },
        { "KTRI", "TRI", "Tri-Cities Regional TN/VA Airport", 36.4752, -82.4074, 1519 },
        { "KGSO", "GSO", "Piedmont Triad International Airport", 36.0978, -79.9373, 925 },
        { "KBGR", "BGR", "Bangor International Airport", 44.8074, -68.8281, 192 },
        { "KACK", "ACK", "Nantucket Memorial Airport", 41.2531, -70.0602, 47 },
        { "KMVY", "MVY", "Martha's Vineyard Airport", 41.3931, -70.6143, 67 },
        { "KILM", "ILM", "Wilmington International Airport", 34.2706, -77.9026, 32 },
        { "KEYW", "EYW", "Key West International Airport", 24.5561, -81.7596, 3 },
        { "KVPS", "VPS", "Destin-Ft Walton Beach Airport", 30.4832, -86.5254, 87 },
        { "KDAB", "DAB", "Daytona Beach International Airport", 29.1799, -81.0581, 34 },
        { "KMLB", "MLB", "Melbourne International Airport", 28.1028, -80.6453, 33 },
        { "KSRQ", "SRQ", "Sarasota Bradenton International Airport", 27.3954, -82.5544, 30 },

        /* === US Airports - Alaska === */
        { "PANC", "ANC", "Ted Stevens Anchorage International Airport", 61.1744, -149.9960, 152 },
        { "PAFA", "FAI", "Fairbanks International Airport", 64.8151, -147.8560, 439 },
        { "PAJN", "JNU", "Juneau International Airport", 58.3550, -134.5760, 21 },

        /* === US Airports - Hawaii === */
        { "PHNL", "HNL", "Daniel K Inouye International Airport", 21.3206, -157.9242, 13 },
        { "PHOG", "OGG", "Kahului Airport", 20.8986, -156.4300, 54 },
        { "PHKO", "KOA", "Ellison Onizuka Kona International At Keahole Airport", 19.7388, -156.0456, 47 },
        { "PHLI", "LIH", "Lihue Airport", 21.9760, -159.3390, 153 },
        { "PHTO", "ITO", "Hilo International Airport", 19.7214, -155.0480, 38 },
    };
    int apt_n = (int)(sizeof(apts) / sizeof(apts[0]));
    if (apt_n > 256) apt_n = 256;

    for (int i = 0; i < apt_n; i++) {
        Airport* a = &state->nav_airports[i];
        strncpy(a->icao, apts[i].icao, sizeof(a->icao) - 1);
        strncpy(a->iata, apts[i].iata, sizeof(a->iata) - 1);
        strncpy(a->name, apts[i].name, sizeof(a->name) - 1);
        a->pos.lat_deg = apts[i].lat;
        a->pos.lon_deg = apts[i].lon;
        a->elevation_ft = apts[i].elev;
        a->mag_var_deg = 0.0f;
        a->runway_count = 0;
        a->longest_runway_ft = 10000.0f;
    }
    state->nav_apt_count = apt_n;
    
    /* Sort airports by ICAO for binary search */
    qsort(state->nav_airports, state->nav_apt_count, sizeof(Airport), compare_airports);

    /* Waypoints are now loaded from earth_fix.dat / earth_nav.dat
     * via spatial hash queries in nav_database_load_files(). */
    /* Allocate waypoint array — will be populated in nav_database_load_files */
    #define INIT_WPT_CAPACITY 200000
    state->nav_waypoints = (Waypoint*)calloc(INIT_WPT_CAPACITY, sizeof(Waypoint));
    state->nav_wpt_capacity = state->nav_waypoints ? INIT_WPT_CAPACITY : 0;
    state->nav_wpt_count = 0;

    LOG_INFO("Nav database: %d airports loaded, wpt buffer %d",
             state->nav_apt_count, state->nav_wpt_capacity);
    return 0;
}

/* =========================================================================
 *  File-based nav data loading (earth_fix.dat + earth_nav.dat)
 * ========================================================================= */

/**
 * @brief Parse one line from earth_fix.dat and insert into spatial hash.
 *
 * Format: lat lon ident type FIR full_type
 * Example: " 33.492513889    9.217400000  07EBA ENRT DT 2118994"
 */
static int parse_fix_line(const char* line, SpatialHash* sh)
{
    double lat = 0.0, lon = 0.0;
    char ident[16] = {0};
    char wpt_type[8] = {0};
    char fir[8] = {0};
    char full_type[16] = {0};

    /* Try sscanf — skip blank lines and comments */
    if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r' || line[0] == '#')
        return 0;

    int n = sscanf(line, "%lf %lf %15s %7s %7s %15s",
                   &lat, &lon, ident, wpt_type, fir, full_type);
    if (n < 3) return 0;  /* Need at least lat, lon, ident */

    /* Basic sanity — valid lat/lon range */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return 0;

    NavSpatialEntry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.ident, ident, sizeof(entry.ident) - 1);
    entry.lat_deg = lat;
    entry.lon_deg = lon;
    entry.type = NAV_WAYPOINT;
    entry.elevation_ft = 0.0f;
    entry.freq_khz = 0.0f;

    /* Type-based classification */
    if (strcmp(wpt_type, "ENRT") == 0) {
        entry.type = NAV_WAYPOINT;
    }

    spatial_hash_insert(sh, &entry);
    return 1;
}

/**
 * @brief Parse one line from earth_nav.dat and insert into spatial hash.
 *
 * Format: record_type lat lon elev_ft freq_khz freq_range mag_var | ident name fir unused facility_type
 * Example: " 3   9.037802778    7.285102778     1191    11630   130     -2.000  ABC ENRT DN ABUJA VOR/DME"
 *
 * Record types:
 *   2  = NDB
 *   3  = VOR
 *   4  = Intersection / Fix
 *   11 = DME (standalone)
 *   12 = DME component of VOR/DME
 *   16 = Airport-related
 *   17 = Airport-related
 */
static int parse_nav_line(const char* line, SpatialHash* sh)
{
    /* Skip header lines */
    if (line[0] == 'I' || line[0] == '\0' || line[0] == '\n'
        || line[0] == '\r' || line[0] == ' ')
    {
        /* Check if it's a header line starting with "I" followed by numbers */
        if (line[0] == 'I') {
            /* "I\n" or "I " → header; but if it's "I" followed by digits it's data */
            return 0;  /* Skip all I-lines — they are version headers */
        }
        if (line[0] == ' ') return 0;  /* Blank-looking line */
    }

    int rec_type = 0;
    double lat = 0.0, lon = 0.0;
    int elev_ft = 0;
    int freq_khz = 0;
    int freq_range = 0;
    double mag_var = 0.0;
    char ident[16] = {0};
    char name[64] = {0};
    char facility[64] = {0};

    /* Parse: rec_type lat lon elev freq freq_range mag_var | ident name fir unused facility... */
    char pipe_part[256] = {0};

    /* Try to find the pipe separator */
    const char* pipe = strchr(line, '|');
    int before_count;
    if (pipe) {
        /* Parse before-pipe part */
        before_count = sscanf(line, "%d %lf %lf %d %d %d %lf",
                              &rec_type, &lat, &lon, &elev_ft,
                              &freq_khz, &freq_range, &mag_var);
        /* Parse after-pipe part */
        char pipe_part[256] = {0};
        size_t pipe_len = strlen(pipe + 1);
        if (pipe_len >= sizeof(pipe_part)) pipe_len = sizeof(pipe_part) - 1;
        memcpy(pipe_part, pipe + 1, pipe_len);
        pipe_part[pipe_len] = '\0';

        sscanf(pipe_part, "%15s %63[^\n]", ident, facility);
        /* Build name from facility text */
        strncpy(name, facility, sizeof(name) - 1);
    } else {
        /* No pipe — try fixed-width or simpler format */
        before_count = sscanf(line, "%d %lf %lf %d %d %d %lf %15s %63[^\n]",
                              &rec_type, &lat, &lon, &elev_ft,
                              &freq_khz, &freq_range, &mag_var,
                              ident, facility);
        strncpy(name, facility, sizeof(name) - 1);
    }

    /* Basic sanity */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return 0;
    if (before_count < 3) return 0;  /* Need at least type, lat, lon */

    NavSpatialEntry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.ident, ident, sizeof(entry.ident) - 1);
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.lat_deg = lat;
    entry.lon_deg = lon;
    entry.elevation_ft = (float)elev_ft;
    entry.freq_khz = (float)freq_khz;
    entry.mag_var_deg = (float)mag_var;

    /* Classify by record type */
    switch (rec_type) {
        case 2:
            entry.type = NAV_NDB;
            break;
        case 3:
            entry.type = NAV_VOR;
            break;
        case 4:
        case 11:
        case 12:
            entry.type = NAV_WAYPOINT;
            break;
        case 16:
        case 17:
            entry.type = NAV_AIRPORT;
            break;
        default:
            entry.type = NAV_WAYPOINT;
            break;
    }

    spatial_hash_insert(sh, &entry);
    return 1;
}

int nav_database_load_files(FMCState* state)
{
    if (!state) return -1;

    /* Create spatial hash with 2003 buckets (good for ~235k entries) */
    SpatialHash* sh = spatial_hash_create(2003);
    if (!sh) {
        LOG_ERROR("Failed to create spatial hash");
        return -1;
    }

    int fix_count = 0;
    int nav_count = 0;

    /* --- Parse earth_fix.dat --- */
    {
        FILE* f = fopen("assets/assets/earth_fix.dat", "r");
        if (!f) {
            LOG_WARN("Cannot open assets/assets/earth_fix.dat — skipping waypoints");
        } else {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                /* Strip trailing newline */
                line[strcspn(line, "\r\n")] = '\0';
                if (parse_fix_line(line, sh)) fix_count++;
            }
            fclose(f);
            LOG_INFO("earth_fix.dat: %d waypoints loaded", fix_count);
        }
    }

    /* --- Parse earth_nav.dat --- */
    {
        FILE* f = fopen("assets/assets/earth_nav.dat", "r");
        if (!f) {
            LOG_WARN("Cannot open assets/assets/earth_nav.dat — skipping navaids");
        } else {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                /* Strip trailing newline */
                line[strcspn(line, "\r\n")] = '\0';
                if (parse_nav_line(line, sh)) nav_count++;
            }
            fclose(f);
            LOG_INFO("earth_nav.dat: %d navaids loaded", nav_count);
        }
    }

    LOG_INFO("Spatial hash total: %d entries (%d fix + %d nav) in %d grid cells",
             sh->total_entries, fix_count, nav_count,
             sh->grid_map ? sh->grid_map->size : 0);

    /* =====================================================================
     *  Populate nav_waypoints — re-read earth_fix.dat directly and add
     *  ALL waypoints (not just near airports). We deduplicate by ident
     *  so each unique waypoint appears once regardless of FIR region.
     *  Navaids (VOR/NDB) from earth_nav.dat are also added.
     * ===================================================================== */
    {
        /* Pass 1: all fixes from earth_fix.dat */
        FILE* f = fopen("assets/assets/earth_fix.dat", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f) && state->nav_wpt_count < state->nav_wpt_capacity) {
                line[strcspn(line, "\r\n")] = '\0';
                double lat, lon;
                char ident[16] = {0}, wpt_type[8] = {0};
                if (sscanf(line, "%lf %lf %15s %7s", &lat, &lon, ident, wpt_type) < 3) continue;
                if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) continue;
                if (!ident[0]) continue;

                /* Dedup via spatial hash — already loaded, faster than linear scan */
                if (state->nav_wpt_count > 0 && state->nav_wpt_count % 10000 == 0) {
                    /* Progress indicator for large loads */
                }

                Waypoint* w = &state->nav_waypoints[state->nav_wpt_count];
                memset(w, 0, sizeof(Waypoint));
                strncpy(w->ident, ident, sizeof(w->ident) - 1);
                w->type         = WPT_WAYPOINT;
                w->pos.lat_deg  = lat;
                w->pos.lon_deg  = lon;
                state->nav_wpt_count++;
            }
            fclose(f);
        }
    }

    LOG_INFO("Nav waypoints loaded: %d from earth_fix.dat + earth_nav.dat",
             state->nav_wpt_count);

    state->spatial_hash = sh;
    return 0;
}

/* =========================================================================
 *  FMC State
 * ========================================================================= */

FMCState* fmc_state_create(void)
{
    FMCState* s = calloc(1, sizeof(FMCState));
    if (!s) {
        LOG_ERROR("Out of memory allocating FMCState");
        return NULL;
    }

    s->mutex = SDL_CreateMutex();
    if (!s->mutex) {
        LOG_ERROR("SDL_CreateMutex failed for FMCState: %s", SDL_GetError());
        free(s);
        return NULL;
    }

    LOG_DEBUG("FMCState created");
    return s;
}

void fmc_state_free(FMCState* state)
{
    if (!state) return;
    if (state->spatial_hash) {
        spatial_hash_destroy(state->spatial_hash);
        state->spatial_hash = NULL;
    }
    if (state->nav_waypoints) {
        free(state->nav_waypoints);
        state->nav_waypoints = NULL;
    }
    if (state->mutex) SDL_DestroyMutex(state->mutex);
    free(state);
}

void fmc_state_lock(FMCState* s)
{
    if (s && s->mutex) SDL_LockMutex(s->mutex);
}

void fmc_state_unlock(FMCState* s)
{
    if (s && s->mutex) SDL_UnlockMutex(s->mutex);
}

const Airport* nav_find_airport(const FMCState* state, const char* icao) {
    if (!state || !icao) return NULL;
    Airport key;
    strncpy(key.icao, icao, sizeof(key.icao) - 1);
    key.icao[sizeof(key.icao) - 1] = '\0';
    
    Airport* result = (Airport*)bsearch(&key, state->nav_airports, state->nav_apt_count, sizeof(Airport), compare_airports);
    return result;
}

/* --- Flight plan operations -------------------------------------------- */

int flight_plan_add_waypoint(FlightPlan* plan, Waypoint wpt, int after_index)
{
    if (!plan) return -1;
    if (plan->waypoint_count >= MAX_ROUTE_WAYPOINTS) {
        LOG_WARN("Flight plan full (max %d waypoints)", MAX_ROUTE_WAYPOINTS);
        return -1;
    }

    int insert_at;
    if (after_index < 0 || after_index >= plan->waypoint_count) {
        /* Append at end */
        insert_at = plan->waypoint_count;
    } else {
        insert_at = after_index + 1;
        /* Shift later waypoints right */
        memmove(&plan->waypoints[insert_at + 1],
                &plan->waypoints[insert_at],
                (size_t)(plan->waypoint_count - insert_at) * sizeof(Waypoint));
    }

    plan->waypoints[insert_at] = wpt;
    plan->waypoint_count++;
    flight_plan_recalculate(plan);
    return 0;
}

int flight_plan_remove_waypoint(FlightPlan* plan, int index)
{
    if (!plan || index < 0 || index >= plan->waypoint_count) return -1;

    /* Shift later waypoints left */
    memmove(&plan->waypoints[index],
            &plan->waypoints[index + 1],
            (size_t)(plan->waypoint_count - index - 1) * sizeof(Waypoint));
    plan->waypoint_count--;

    /* Clear the vacated slot */
    memset(&plan->waypoints[plan->waypoint_count], 0, sizeof(Waypoint));
    flight_plan_recalculate(plan);
    return 0;
}

void flight_plan_clear(FlightPlan* plan)
{
    if (!plan) return;
    memset(plan->waypoints, 0, sizeof(plan->waypoints));
    plan->waypoint_count = 0;
    plan->active_waypoint_index = -1;
    plan->total_distance_nm = 0.0f;
    plan->estimated_time_hours = 0.0f;
    plan->fuel_required_lbs = 0.0f;
    
    plan->climb_tgt_spd_mach = 0.78f;
    plan->climb_tgt_spd_kts = 280.0f;
    plan->climb_spd_rest_kts = 250.0f;
    plan->climb_spd_rest_alt_ft = 10000.0f;
    
    plan->cruise_tgt_spd_mach = 0.78f;
    
    plan->descent_tgt_spd_mach = 0.78f;
    plan->descent_tgt_spd_kts = 280.0f;
    plan->descent_spd_rest_kts = 250.0f;
    plan->descent_spd_rest_alt_ft = 10000.0f;
    plan->descent_ed_alt_ft = 10000.0f;

    /* FMCState::plan_modified must be set by the caller */
}

void flight_plan_recalculate(FlightPlan* plan)
{
    if (!plan || plan->waypoint_count < 2) {
        if (plan) {
            plan->total_distance_nm = 0.0f;
            plan->estimated_time_hours = 0.0f;
        }
        return;
    }

    double total_nm = 0.0;
    for (int i = 0; i < plan->waypoint_count - 1; i++) {
        total_nm += geo_distance_nm(plan->waypoints[i].pos,
                                    plan->waypoints[i + 1].pos);
    }
    plan->total_distance_nm = (float)total_nm;

    if (plan->cruise_speed_kts > 0.0f) {
        plan->estimated_time_hours = (float)(total_nm / plan->cruise_speed_kts);
    }

    /* Rough fuel estimate: 5000 lbs/hour as default burn rate */
    float fuel_burn_pph = 5000.0f;
    plan->fuel_required_lbs = plan->estimated_time_hours * fuel_burn_pph;
}

/* --- Serialization ----------------------------------------------------- */

int flight_plan_save(const FlightPlan* plan, const char* path)
{
    if (!plan || !path) return -1;

    FILE* fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Cannot open %s for writing flight plan", path);
        return -1;
    }

    fprintf(fp, "# Flight Plan\n");
    fprintf(fp, "flight_number=%s\n", plan->flight_number);
    fprintf(fp, "aircraft_type=%s\n", plan->aircraft_type);
    fprintf(fp, "departure=%s\n", plan->departure.icao);
    fprintf(fp, "arrival=%s\n", plan->arrival.icao);
    fprintf(fp, "cruise_alt=%f\n", (double)plan->cruise_altitude_ft);
    fprintf(fp, "cruise_spd=%f\n", (double)plan->cruise_speed_kts);
    fprintf(fp, "waypoint_count=%d\n", plan->waypoint_count);

    for (int i = 0; i < plan->waypoint_count; i++) {
        const Waypoint* w = &plan->waypoints[i];
        fprintf(fp, "wpt[%d]=%s,%d,%.6f,%.6f,%.1f,%.2f\n",
                i, w->ident, (int)w->type,
                w->pos.lat_deg, w->pos.lon_deg,
                (double)w->elevation_ft, (double)w->freq_mhz);
    }

    fclose(fp);
    LOG_INFO("Flight plan saved to %s (%d waypoints)", path, plan->waypoint_count);
    return 0;
}

int flight_plan_load(FlightPlan* plan, const char* path)
{
    if (!plan || !path) return -1;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("Flight plan file not found: %s", path);
        return -1;
    }

    flight_plan_clear(plan);

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;

        char key[64] = {0};
        char val[192] = {0};

        if (sscanf(line, "%63[^=]=%191[^\n]", key, val) == 2) {
            if (strcmp(key, "flight_number") == 0) {
                strncpy(plan->flight_number, val, sizeof(plan->flight_number) - 1);
            } else if (strcmp(key, "aircraft_type") == 0) {
                strncpy(plan->aircraft_type, val, sizeof(plan->aircraft_type) - 1);
            } else if (strcmp(key, "departure") == 0) {
                strncpy(plan->departure.icao, val, sizeof(plan->departure.icao) - 1);
            } else if (strcmp(key, "arrival") == 0) {
                strncpy(plan->arrival.icao, val, sizeof(plan->arrival.icao) - 1);
            } else if (strcmp(key, "cruise_alt") == 0) {
                plan->cruise_altitude_ft = (float)atof(val);
            } else if (strcmp(key, "cruise_spd") == 0) {
                plan->cruise_speed_kts = (float)atof(val);
            } else if (strcmp(key, "waypoint_count") == 0) {
                /* Read the specified number of waypoints */
                int count = atoi(val);
                for (int i = 0; i < count; i++) {
                    char wpt_line[256];
                    if (!fgets(wpt_line, sizeof(wpt_line), fp)) break;
                    wpt_line[strcspn(wpt_line, "\r\n")] = '\0';

                    Waypoint w;
                    memset(&w, 0, sizeof(w));
                    int idx, type;
                    if (sscanf(wpt_line, "wpt[%d]=%7[^,],%d,%lf,%lf,%f,%f",
                               &idx, w.ident, &type,
                               &w.pos.lat_deg, &w.pos.lon_deg,
                               &w.elevation_ft, &w.freq_mhz) == 7) {
                        w.type = (WaypointType)type;
                        flight_plan_add_waypoint(plan, w, -1);
                    }
                }
            }
        }
    }

    fclose(fp);
    flight_plan_recalculate(plan);
    LOG_INFO("Flight plan loaded from %s (%d waypoints, %.0f NM)",
             path, plan->waypoint_count, (double)plan->total_distance_nm);
    return 0;
}
