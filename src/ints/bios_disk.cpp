/*
 *  Copyright (C) 2002-2020  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include "dosbox.h"
#include "callback.h"
#include "bios.h"
#include "bios_disk.h"
#include "regs.h"
#include "mem.h"
#include "dos_inc.h" /* for Drives[] */
#include "../dos/drives.h"
#include "mapper.h"
#include "timer.h"
#include <ctype.h>
#include <map>
#include <string>
#include <vector>


struct fatFromDOSDrive {
    enum {
        BYTES_PER_SECTOR=512,
        HEAD_COUNT=255,
        SECTORS_PER_TRACK=63,
        PARTITION_START=32,
        ROOT_ENTRIES=512,
        OPEN_CACHE=8
    };

    struct Node {
        std::string name;
        std::string path;
        Bit32u size;
        Bit16u date,time;
        Bit8u attr;
        Bit32u first_cluster;
        std::vector<Node *> children;
        Node() : size(0),date(0),time(0),attr(0),first_cluster(0) {}
        ~Node() { for (size_t i=0;i<children.size();i++) delete children[i]; }
    };
    struct Cluster {
        Node *file;
        Bit32u file_offset;
        std::vector<Bit8u> data;
        Cluster() : file(NULL),file_offset(0) {}
    };
    struct OpenFile {
        Node *node;
        DOS_File *file;
        OpenFile() : node(NULL),file(NULL) {}
    };

    DOS_Drive *drive;
    unsigned letter;
    bool read_only;
    bool success;
    bool timed_out;
    Bit32u deadline;
    Bit32u sectors_per_cluster;
    Bit32u total_clusters;
    Bit32u fat_sectors;
    Bit32u fat1_start,fat2_start,root_start,data_start,disk_end;
    Node *root_node;
    std::vector<Bit8u> mbr,boot,fat,root;
    std::vector<Cluster> clusters;
    std::map<Bit32u,std::vector<Bit8u> > overlay;
    OpenFile open_files[OPEN_CACHE];
    unsigned open_cursor;

    static void put16(Bit8u *p,Bit16u v) { p[0]=(Bit8u)v; p[1]=(Bit8u)(v>>8); }
    static void put32(Bit8u *p,Bit32u v) { p[0]=(Bit8u)v; p[1]=(Bit8u)(v>>8); p[2]=(Bit8u)(v>>16); p[3]=(Bit8u)(v>>24); }

    fatFromDOSDrive(DOS_Drive *drv,unsigned drv_letter,Bit32u free_mb,int timeout,bool ro)
        : drive(drv),letter(drv_letter),read_only(ro),success(false),timed_out(false),
          deadline(timeout>0 ? GetTicks()+(Bit32u)timeout*1000u : 0),
          sectors_per_cluster(1),total_clusters(0),fat_sectors(0),
          fat1_start(0),fat2_start(0),root_start(0),data_start(0),disk_end(0),
          root_node(new Node),open_cursor(0)
    {
        root_node->attr=DOS_ATTR_DIRECTORY;
        if (!enumerate_dir(root_node,"")) {
            if (timed_out) LOG_MSG("BOOT: FAT conversion of drive %c: timed out",'A'+letter);
            return;
        }

        choose_geometry(free_mb);
        if (!allocate_tree()) return;
        build_filesystem();
        success=true;
        LOG_MSG("BOOT: mounted directory %c: converted to virtual FAT16 HDD (%lu MiB, %lu KiB clusters%s)",
                'A'+letter,(unsigned long)((disk_end+2047u)/2048u),
                (unsigned long)(sectors_per_cluster/2u),read_only?", read-only":"");
    }

    ~fatFromDOSDrive() {
        for (unsigned i=0;i<OPEN_CACHE;i++) close_slot(i);
        delete root_node;
    }

    bool expired() {
        if (!deadline) return false;
        if ((Bit32s)(GetTicks()-deadline)<0) return false;
        timed_out=true;
        return true;
    }

    bool enumerate_dir(Node *parent,const std::string &path) {
        if (expired()) return false;
        struct Found {
            std::string name;
            Bit32u size;
            Bit16u date,time;
            Bit8u attr;
        };
        std::vector<Found> found;
        RealPt save_dta=dos.dta();
        dos.dta(dos.tables.tempdta);
        DOS_DTA dta(dos.dta());
        char pattern[4]={'*','.','*',0};
        dta.SetupSearch(255,0xff,pattern);
        char finddir[DOS_PATHLENGTH+1];
        safe_strncpy(finddir,path.c_str(),sizeof(finddir));
        bool more=drive->FindFirst(finddir,dta,false);
        while (more) {
            char name[DOS_NAMELENGTH_ASCII];
            Found f;
            dta.GetResult(name,f.size,f.date,f.time,f.attr);
            f.name=name;
            if (f.name!="." && f.name!=".." && !(f.attr&DOS_ATTR_VOLUME))
                found.push_back(f);
            if (expired()) { dos.dta(save_dta); return false; }
            more=drive->FindNext(dta);
        }
        dos.dta(save_dta);

        for (size_t i=0;i<found.size();i++) {
            Node *n=new Node;
            n->name=found[i].name;
            n->path=path.empty()?n->name:path+"\\"+n->name;
            n->size=found[i].size;
            n->date=found[i].date;
            n->time=found[i].time;
            n->attr=found[i].attr;
            parent->children.push_back(n);
        }
        for (size_t i=0;i<parent->children.size();i++) {
            Node *n=parent->children[i];
            if ((n->attr&DOS_ATTR_DIRECTORY) && !enumerate_dir(n,n->path)) return false;
        }
        return true;
    }

    Bit32u estimate_clusters(const Node *dir,Bit32u cluster_bytes,bool root_dir) const {
        Bit32u result=0;
        if (!root_dir) {
            const Bit32u entries=(Bit32u)dir->children.size()+2u;
            result+=(entries*32u+cluster_bytes-1u)/cluster_bytes;
        }
        for (size_t i=0;i<dir->children.size();i++) {
            const Node *n=dir->children[i];
            if (n->attr&DOS_ATTR_DIRECTORY) result+=estimate_clusters(n,cluster_bytes,false);
            else if (n->size) result+=(n->size+cluster_bytes-1u)/cluster_bytes;
        }
        return result;
    }

    void choose_geometry(Bit32u free_mb) {
        const Bit64u free_bytes=(Bit64u)free_mb*1024u*1024u;
        for (sectors_per_cluster=1;sectors_per_cluster<64;sectors_per_cluster<<=1) {
            const Bit32u cluster_bytes=sectors_per_cluster*BYTES_PER_SECTOR;
            const Bit64u used=estimate_clusters(root_node,cluster_bytes,true);
            Bit64u wanted=used+(free_bytes+cluster_bytes-1u)/cluster_bytes;
            if (wanted<4085u) wanted=4085u;
            if (wanted<=65524u) { total_clusters=(Bit32u)wanted; return; }
        }
        sectors_per_cluster=64;
        const Bit32u cluster_bytes=sectors_per_cluster*BYTES_PER_SECTOR;
        Bit64u wanted=estimate_clusters(root_node,cluster_bytes,true)+(free_bytes+cluster_bytes-1u)/cluster_bytes;
        if (wanted<4085u) wanted=4085u;
        if (wanted>65524u) wanted=65524u;
        total_clusters=(Bit32u)wanted;
    }

    Bit32u allocate_chain(Bit32u count,Node *file) {
        if (!count) return 0;
        const Bit32u first=(Bit32u)clusters.size()+2u;
        if ((Bit64u)first+count-2u>total_clusters) return 0;
        for (Bit32u i=0;i<count;i++) {
            Cluster c;
            c.file=file;
            if (file) c.file_offset=i*sectors_per_cluster*BYTES_PER_SECTOR;
            else c.data.resize(sectors_per_cluster*BYTES_PER_SECTOR,0);
            clusters.push_back(c);
        }
        return first;
    }

    bool allocate_nodes(Node *dir,bool is_root) {
        const Bit32u cluster_bytes=sectors_per_cluster*BYTES_PER_SECTOR;
        if (!is_root) {
            const Bit32u count=((Bit32u)dir->children.size()+2u)*32u;
            dir->first_cluster=allocate_chain((count+cluster_bytes-1u)/cluster_bytes,NULL);
            if (!dir->first_cluster) return false;
        }
        for (size_t i=0;i<dir->children.size();i++) {
            Node *n=dir->children[i];
            if (n->attr&DOS_ATTR_DIRECTORY) {
                if (!allocate_nodes(n,false)) return false;
            } else if (n->size) {
                n->first_cluster=allocate_chain((n->size+cluster_bytes-1u)/cluster_bytes,n);
                if (!n->first_cluster) return false;
            }
        }
        return true;
    }

    static void make_83(Bit8u out[11],const std::string &name) {
        memset(out,' ',11);
        if (name==".") { out[0]='.'; return; }
        if (name=="..") { out[0]='.'; out[1]='.'; return; }
        const size_t dot=name.find_last_of('.');
        const size_t base_end=(dot==std::string::npos?name.size():dot);
        size_t j=0;
        for (size_t i=0;i<base_end && j<8;i++) {
            unsigned char ch=(unsigned char)name[i];
            if (ch==' ' || ch=='.') continue;
            out[j++]=(Bit8u)toupper(ch);
        }
        if (dot!=std::string::npos) {
            j=8;
            for (size_t i=dot+1;i<name.size() && j<11;i++) {
                unsigned char ch=(unsigned char)name[i];
                if (ch==' ' || ch=='.') continue;
                out[j++]=(Bit8u)toupper(ch);
            }
        }
    }

    void write_entry(Bit8u *dst,const Node *n,const char *special=NULL,Bit32u parent_cluster=0) {
        memset(dst,0,32);
        make_83(dst,special?special:n->name);
        Bit8u attr=special?(Bit8u)DOS_ATTR_DIRECTORY:n->attr;
        attr&=(DOS_ATTR_READ_ONLY|DOS_ATTR_HIDDEN|DOS_ATTR_SYSTEM|DOS_ATTR_DIRECTORY|DOS_ATTR_ARCHIVE);
        if (!read_only) attr&=~DOS_ATTR_READ_ONLY;
        dst[11]=attr;
        put16(dst+14,n?n->time:0);
        put16(dst+16,n?n->date:0);
        put16(dst+18,n?n->date:0);
        put16(dst+22,n?n->time:0);
        put16(dst+24,n?n->date:0);
        Bit32u cluster=special ? (special[1]=='.'?parent_cluster:(n?n->first_cluster:0)) : n->first_cluster;
        put16(dst+26,(Bit16u)cluster);
        put32(dst+28,(n && !(attr&DOS_ATTR_DIRECTORY))?n->size:0);
    }

    void fill_directory(Node *dir,bool is_root,Bit32u parent_cluster) {
        Bit8u *bytes=NULL;
        Bit32u byte_count=0;
        if (is_root) {
            bytes=&root[0]; byte_count=(Bit32u)root.size();
        } else {
            Cluster &first=clusters[dir->first_cluster-2u];
            const Bit32u count=((Bit32u)dir->children.size()+2u)*32u;
            const Bit32u chain_count=(count+sectors_per_cluster*BYTES_PER_SECTOR-1u)/(sectors_per_cluster*BYTES_PER_SECTOR);
            std::vector<Bit8u> combined(chain_count*sectors_per_cluster*BYTES_PER_SECTOR,0);
            write_entry(&combined[0],dir,".",parent_cluster);
            write_entry(&combined[32],dir,"..",parent_cluster);
            for (size_t i=0;i<dir->children.size();i++) write_entry(&combined[(i+2u)*32u],dir->children[i]);
            for (Bit32u i=0;i<chain_count;i++) {
                Cluster &cl=clusters[dir->first_cluster-2u+i];
                memcpy(&cl.data[0],&combined[i*sectors_per_cluster*BYTES_PER_SECTOR],sectors_per_cluster*BYTES_PER_SECTOR);
            }
            bytes=&first.data[0]; byte_count=(Bit32u)first.data.size();
            (void)bytes; (void)byte_count;
        }
        if (is_root) {
            for (size_t i=0;i<dir->children.size();i++) write_entry(&root[i*32u],dir->children[i]);
        }
        for (size_t i=0;i<dir->children.size();i++) {
            Node *n=dir->children[i];
            if (n->attr&DOS_ATTR_DIRECTORY) fill_directory(n,false,is_root?0:dir->first_cluster);
        }
    }

    bool allocate_tree() {
        if (root_node->children.size()>ROOT_ENTRIES) {
            LOG_MSG("BOOT: drive %c: has too many FAT16 root entries (%lu > %u)",'A'+letter,(unsigned long)root_node->children.size(),ROOT_ENTRIES);
            return false;
        }
        if (!allocate_nodes(root_node,true)) {
            LOG_MSG("BOOT: drive %c: does not fit in converted FAT16 image",'A'+letter);
            return false;
        }
        root.resize(ROOT_ENTRIES*32u,0);
        fill_directory(root_node,true,0);
        return true;
    }

    void set_fat(Bit32u cluster,Bit16u value) {
        const Bit32u off=cluster*2u;
        if (off+1u<fat.size()) put16(&fat[off],value);
    }

    static void encode_chs(Bit8u out[3],Bit32u lba) {
        Bit32u cylinder=lba/(HEAD_COUNT*SECTORS_PER_TRACK);
        const Bit32u rem=lba%(HEAD_COUNT*SECTORS_PER_TRACK);
        const Bit32u head=rem/SECTORS_PER_TRACK;
        const Bit32u sector=rem%SECTORS_PER_TRACK+1u;
        if (cylinder>1023u) cylinder=1023u;
        out[0]=(Bit8u)head;
        out[1]=(Bit8u)((sector&0x3fu)|((cylinder>>2)&0xc0u));
        out[2]=(Bit8u)cylinder;
    }

    void build_filesystem() {
        fat_sectors=((total_clusters+2u)*2u+BYTES_PER_SECTOR-1u)/BYTES_PER_SECTOR;
        fat1_start=PARTITION_START+1u;
        fat2_start=fat1_start+fat_sectors;
        root_start=fat2_start+fat_sectors;
        data_start=root_start+(ROOT_ENTRIES*32u)/BYTES_PER_SECTOR;
        disk_end=data_start+total_clusters*sectors_per_cluster;

        fat.resize(fat_sectors*BYTES_PER_SECTOR,0);
        set_fat(0,0xfff8u); set_fat(1,0xffffu);
        for (Bit32u i=0;i<clusters.size();i++) {
            const Bit32u cluster=i+2u;
            bool chain_next=false;
            if (i+1u<clusters.size()) {
                const Cluster &cur=clusters[i],&next=clusters[i+1u];
                if (cur.file && next.file==cur.file && next.file_offset==cur.file_offset+sectors_per_cluster*BYTES_PER_SECTOR) chain_next=true;
                if (!cur.file && !next.file && cur.data.size() && next.data.size()) {
                    /* Directory clusters are contiguous; only chain while the current
                       directory data was allocated as a multi-cluster block. */
                    chain_next=(cluster+1u<=(Bit32u)clusters.size()+1u &&
                                cur.data.size()==next.data.size());
                    if (chain_next) {
                        /* Stop at the next cluster that starts with a dot entry. */
                        if (next.data.size()>=32 && next.data[0]=='.') chain_next=false;
                    }
                }
            }
            set_fat(cluster,chain_next?(Bit16u)(cluster+1u):0xffffu);
        }
        /* Rebuild exact directory chains (the generic pass above intentionally
           favours ending a chain over accidentally linking two directories). */
        set_directory_chains(root_node,true);

        mbr.resize(BYTES_PER_SECTOR,0);
        Bit8u *pe=&mbr[446];
        pe[0]=0; encode_chs(pe+1,PARTITION_START); pe[4]=0x06;
        encode_chs(pe+5,disk_end-1u);
        put32(pe+8,PARTITION_START);
        put32(pe+12,disk_end-PARTITION_START);
        mbr[510]=0x55; mbr[511]=0xaa;

        boot.resize(BYTES_PER_SECTOR,0);
        boot[0]=0xeb; boot[1]=0x3c; boot[2]=0x90;
        memcpy(&boot[3],"DOSBOX-X",8);
        put16(&boot[11],BYTES_PER_SECTOR);
        boot[13]=(Bit8u)sectors_per_cluster;
        put16(&boot[14],1); boot[16]=2;
        put16(&boot[17],ROOT_ENTRIES);
        const Bit32u part_sectors=disk_end-PARTITION_START;
        if (part_sectors<65536u) put16(&boot[19],(Bit16u)part_sectors);
        else put32(&boot[32],part_sectors);
        boot[21]=0xf8;
        put16(&boot[22],(Bit16u)fat_sectors);
        put16(&boot[24],SECTORS_PER_TRACK);
        put16(&boot[26],HEAD_COUNT);
        put32(&boot[28],PARTITION_START);
        boot[36]=0x80; boot[38]=0x29;
        put32(&boot[39],0x58424f44u ^ (letter<<24));
        char label[12]; sprintf(label,"HOST%c      ",'A'+letter); memcpy(&boot[43],label,11);
        memcpy(&boot[54],"FAT16   ",8);
        boot[510]=0x55; boot[511]=0xaa;
    }

    void set_directory_chains(Node *dir,bool is_root) {
        if (!is_root) {
            const Bit32u cluster_bytes=sectors_per_cluster*BYTES_PER_SECTOR;
            const Bit32u count=(((Bit32u)dir->children.size()+2u)*32u+cluster_bytes-1u)/cluster_bytes;
            for (Bit32u i=0;i<count;i++) set_fat(dir->first_cluster+i,i+1u<count?(Bit16u)(dir->first_cluster+i+1u):0xffffu);
        }
        for (size_t i=0;i<dir->children.size();i++) if (dir->children[i]->attr&DOS_ATTR_DIRECTORY) set_directory_chains(dir->children[i],false);
    }

    void close_slot(unsigned slot) {
        if (open_files[slot].file) {
            open_files[slot].file->Close();
            delete open_files[slot].file;
            open_files[slot].file=NULL;
            open_files[slot].node=NULL;
        }
    }

    DOS_File *open_file(Node *node) {
        for (unsigned i=0;i<OPEN_CACHE;i++) if (open_files[i].node==node) return open_files[i].file;
        const unsigned slot=open_cursor++%OPEN_CACHE;
        close_slot(slot);
        DOS_File *file=NULL;
        char path[DOS_PATHLENGTH+1]; safe_strncpy(path,node->path.c_str(),sizeof(path));
        if (!drive->FileOpen(&file,path,OPEN_READ)) return NULL;
        open_files[slot].node=node; open_files[slot].file=file;
        return file;
    }

    Bit8u read(Bit32u sector,void *data) {
        Bit8u *dst=(Bit8u *)data;
        std::map<Bit32u,std::vector<Bit8u> >::const_iterator it=overlay.find(sector);
        if (it!=overlay.end()) { memcpy(dst,&it->second[0],BYTES_PER_SECTOR); return 0; }
        memset(dst,0,BYTES_PER_SECTOR);
        if (sector==0) { memcpy(dst,&mbr[0],BYTES_PER_SECTOR); return 0; }
        if (sector==PARTITION_START) { memcpy(dst,&boot[0],BYTES_PER_SECTOR); return 0; }
        if (sector>=fat1_start && sector<fat1_start+fat_sectors) { memcpy(dst,&fat[(sector-fat1_start)*BYTES_PER_SECTOR],BYTES_PER_SECTOR); return 0; }
        if (sector>=fat2_start && sector<fat2_start+fat_sectors) { memcpy(dst,&fat[(sector-fat2_start)*BYTES_PER_SECTOR],BYTES_PER_SECTOR); return 0; }
        if (sector>=root_start && sector<data_start) { memcpy(dst,&root[(sector-root_start)*BYTES_PER_SECTOR],BYTES_PER_SECTOR); return 0; }
        if (sector>=data_start && sector<disk_end) {
            const Bit32u rel=sector-data_start;
            const Bit32u cluster_index=rel/sectors_per_cluster;
            const Bit32u sector_in_cluster=rel%sectors_per_cluster;
            if (cluster_index<clusters.size()) {
                Cluster &cl=clusters[cluster_index];
                if (!cl.data.empty()) memcpy(dst,&cl.data[sector_in_cluster*BYTES_PER_SECTOR],BYTES_PER_SECTOR);
                else if (cl.file) {
                    DOS_File *file=open_file(cl.file);
                    if (!file) return 0x04;
                    Bit32u pos=cl.file_offset+sector_in_cluster*BYTES_PER_SECTOR;
                    if (!file->Seek(&pos,DOS_SEEK_SET)) return 0x04;
                    Bit16u count=BYTES_PER_SECTOR;
                    if (!file->Read(dst,&count)) return 0x04;
                    if (count<BYTES_PER_SECTOR) memset(dst+count,0,BYTES_PER_SECTOR-count);
                }
            }
        }
        return 0;
    }

    Bit8u write(Bit32u sector,const void *data) {
        if (read_only) return 0;
        std::vector<Bit8u> &buf=overlay[sector];
        buf.resize(BYTES_PER_SECTOR);
        memcpy(&buf[0],data,BYTES_PER_SECTOR);
        return 0;
    }
};



diskGeo DiskGeometryList[] = {
	{ 160,  8, 1, 40, 0},
	{ 180,  9, 1, 40, 0},
	{ 200, 10, 1, 40, 0},
	{ 320,  8, 2, 40, 1},
	{ 360,  9, 2, 40, 1},
	{ 400, 10, 2, 40, 1},
	{ 720,  9, 2, 80, 3},
	{1200, 15, 2, 80, 2},
	{1440, 18, 2, 80, 4},
	{2880, 36, 2, 80, 6},
	{0, 0, 0, 0, 0}
};

Bitu call_int13;
Bitu diskparm0, diskparm1;
static Bit8u last_status;
static Bit8u last_drive;
Bit16u imgDTASeg;
RealPt imgDTAPtr;
DOS_DTA *imgDTA;
bool killRead;
static bool swapping_requested;

void CMOS_SetRegister(Bitu regNr, Bit8u val); //For setting equipment word

/* 2 floppys and 2 harddrives, max */
imageDisk *imageDiskList[MAX_DISK_IMAGES];
imageDisk *diskSwap[MAX_SWAPPABLE_DISKS];
Bit32s swapPosition;

void updateDPT(void) {
	Bit32u tmpheads, tmpcyl, tmpsect, tmpsize;
	if(imageDiskList[2] != NULL) {
		PhysPt dp0physaddr=CALLBACK_PhysPointer(diskparm0);
		imageDiskList[2]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp0physaddr,(Bit16u)tmpcyl);
		phys_writeb(dp0physaddr+0x2,(Bit8u)tmpheads);
		phys_writew(dp0physaddr+0x3,0);
		phys_writew(dp0physaddr+0x5,(Bit16u)-1);
		phys_writeb(dp0physaddr+0x7,0);
		phys_writeb(dp0physaddr+0x8,(0xc0 | (((imageDiskList[2]->heads) > 8) << 3)));
		phys_writeb(dp0physaddr+0x9,0);
		phys_writeb(dp0physaddr+0xa,0);
		phys_writeb(dp0physaddr+0xb,0);
		phys_writew(dp0physaddr+0xc,(Bit16u)tmpcyl);
		phys_writeb(dp0physaddr+0xe,(Bit8u)tmpsect);
	}
	if(imageDiskList[3] != NULL) {
		PhysPt dp1physaddr=CALLBACK_PhysPointer(diskparm1);
		imageDiskList[3]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		phys_writew(dp1physaddr,(Bit16u)tmpcyl);
		phys_writeb(dp1physaddr+0x2,(Bit8u)tmpheads);
		phys_writeb(dp1physaddr+0xe,(Bit8u)tmpsect);
	}
}

void incrementFDD(void) {
	Bit16u equipment=mem_readw(BIOS_CONFIGURATION);
	if(equipment&1) {
		Bitu numofdisks = (equipment>>6)&3;
		numofdisks++;
		if(numofdisks > 1) numofdisks=1;//max 2 floppies at the moment
		equipment&=~0x00C0;
		equipment|=(numofdisks<<6);
	} else equipment|=1;
	mem_writew(BIOS_CONFIGURATION,equipment);
	if (IS_EGAVGA_ARCH) equipment &= ~0x30; //EGA/VGA startup display mode differs in CMOS
	CMOS_SetRegister(0x14, (Bit8u)(equipment&0xff));
}

void swapInDisks(void) {
	bool allNull = true;
	Bit32s diskcount = 0;
	Bit32s swapPos = swapPosition;
	Bit32s i;

	/* Check to make sure that  there is at least one setup image */
	for(i=0;i<MAX_SWAPPABLE_DISKS;i++) {
		if(diskSwap[i]!=NULL) {
			allNull = false;
			break;
		}
	}

	/* No disks setup... fail */
	if (allNull) return;

	/* If only one disk is loaded, this loop will load the same disk in dive A and drive B */
	while(diskcount<2) {
		if(diskSwap[swapPos] != NULL) {
			LOG_MSG("Loaded disk %d from swaplist position %d - \"%s\"", diskcount, swapPos, diskSwap[swapPos]->diskname);
			imageDiskList[diskcount] = diskSwap[swapPos];
			diskcount++;
		}
		swapPos++;
		if(swapPos>=MAX_SWAPPABLE_DISKS) swapPos=0;
	}
}

bool getSwapRequest(void) {
	bool sreq=swapping_requested;
	swapping_requested = false;
	return sreq;
}

void swapInNextDisk(bool pressed) {
	if (!pressed)
		return;
	DriveManager::CycleAllDisks();
	/* Hack/feature: rescan all disks as well */
	LOG_MSG("Diskcaching reset for normal mounted drives.");
	for(Bitu i=0;i<DOS_DRIVES;i++) {
		if (Drives[i]) Drives[i]->EmptyCache();
	}
	swapPosition++;
	if(diskSwap[swapPosition] == NULL) swapPosition = 0;
	swapInDisks();
	swapping_requested = true;
}


Bit8u imageDisk::Read_Sector(Bit32u head,Bit32u cylinder,Bit32u sector,void * data) {
	Bit32u sectnum;

	sectnum = ( (cylinder * heads + head) * sectors ) + sector - 1L;

	return Read_AbsoluteSector(sectnum, data);
}

Bit8u imageDisk::Read_AbsoluteSector(Bit32u sectnum, void * data) {
	if (ffdd) return ffdd->read(sectnum,data);
	Bit32u bytenum;

	bytenum = sectnum * sector_size;

	if (last_action==WRITE || bytenum!=current_fpos) fseek(diskimg,bytenum,SEEK_SET);
	size_t ret=fread(data, 1, sector_size, diskimg);
	current_fpos=bytenum+ret;
	last_action=READ;

	return 0x00;
}

Bit8u imageDisk::Write_Sector(Bit32u head,Bit32u cylinder,Bit32u sector,void * data) {
	Bit32u sectnum;

	sectnum = ( (cylinder * heads + head) * sectors ) + sector - 1L;

	return Write_AbsoluteSector(sectnum, data);
}


Bit8u imageDisk::Write_AbsoluteSector(Bit32u sectnum, void *data) {
	if (ffdd) return ffdd->write(sectnum,data);
	Bit32u bytenum;

	bytenum = sectnum * sector_size;

	//LOG_MSG("Writing sectors to %ld at bytenum %d", sectnum, bytenum);

	if (last_action==READ || bytenum!=current_fpos) fseek(diskimg,bytenum,SEEK_SET);
	size_t ret=fwrite(data, 1, sector_size, diskimg);
	current_fpos=bytenum+ret;
	last_action=WRITE;

	return ((ret>0)?0x00:0x05);

}

imageDisk::imageDisk(FILE *imgFile, const char *imgName, Bit32u imgSizeK, bool isHardDisk) {
	ffdd = NULL;
	source_drive = 0xff;
	heads = 0;
	cylinders = 0;
	sectors = 0;
	sector_size = 512;
	current_fpos = 0;
	last_action = NONE;
	diskimg = imgFile;
	fseek(diskimg,0,SEEK_SET);
	memset(diskname,0,512);
	safe_strncpy(diskname, imgName, sizeof(diskname));
	active = false;
	hardDrive = isHardDisk;
	if(!isHardDisk) {
		Bit8u i=0;
		bool founddisk = false;
		while (DiskGeometryList[i].ksize!=0x0) {
			if ((DiskGeometryList[i].ksize==imgSizeK) ||
				(DiskGeometryList[i].ksize+1==imgSizeK)) {
				if (DiskGeometryList[i].ksize!=imgSizeK)
					LOG_MSG("ImageLoader: image file with additional data, might not load!");
				founddisk = true;
				active = true;
				floppytype = i;
				heads = DiskGeometryList[i].headscyl;
				cylinders = DiskGeometryList[i].cylcount;
				sectors = DiskGeometryList[i].secttrack;
				break;
			}
			i++;
		}
		if(!founddisk) {
			active = false;
		} else {
			incrementFDD();
		}
	}
}

imageDisk::imageDisk(DOS_Drive *drive, unsigned letter, Bit32u freeMB, int timeout, bool readOnly) {
	ffdd = new fatFromDOSDrive(drive,letter,freeMB,timeout,readOnly);
	source_drive = (Bit8u)letter;
	diskimg = NULL;
	sector_size = 512;
	heads = fatFromDOSDrive::HEAD_COUNT;
	sectors = fatFromDOSDrive::SECTORS_PER_TRACK;
	cylinders = ffdd && ffdd->success ? (ffdd->disk_end + heads*sectors - 1u)/(heads*sectors) : 0;
	if (cylinders>1024u) cylinders=1024u;
	current_fpos=0; last_action=NONE; floppytype=0;
	memset(diskname,0,sizeof(diskname));
	sprintf(diskname,"host directory %c:",'A'+letter);
	active=ffdd && ffdd->success;
	hardDrive=true;
}

imageDisk::~imageDisk() {
	delete ffdd;
	ffdd=NULL;
	if (diskimg) fclose(diskimg);
}

bool imageDisk::IsConvertedFAT(void) const { return ffdd!=NULL; }

Bitu BIOS_ConvertMountedDriveToFAT(Bit32u freeMB,int timeout,bool readOnly,Bit8u skipDrive,Bit8u *sourceDrive) {
	const unsigned slot=3;
	if (imageDiskList[slot] && !imageDiskList[slot]->IsConvertedFAT()) return 0;
	if (imageDiskList[slot]) { delete imageDiskList[slot]; imageDiskList[slot]=NULL; }
	for (unsigned drv=2;drv<25;drv++) {
		if (drv==skipDrive || !Drives[drv]) continue;
		const char *info=Drives[drv]->GetInfo();
		if (info && (!strncasecmp(info,"fatDrive",8) || !strncasecmp(info,"isoDrive",8))) continue;
		imageDisk *disk=new imageDisk(Drives[drv],drv,freeMB,timeout,readOnly);
		if (!disk->active) { delete disk; continue; }
		imageDiskList[slot]=disk;
		updateDPT();
		if (sourceDrive) *sourceDrive=(Bit8u)drv;
		return 1;
	}
	return 0;
}

void imageDisk::Set_Geometry(Bit32u setHeads, Bit32u setCyl, Bit32u setSect, Bit32u setSectSize) {
	heads = setHeads;
	cylinders = setCyl;
	sectors = setSect;
	sector_size = setSectSize;
	active = true;
}

void imageDisk::Get_Geometry(Bit32u * getHeads, Bit32u *getCyl, Bit32u *getSect, Bit32u *getSectSize) {
	*getHeads = heads;
	*getCyl = cylinders;
	*getSect = sectors;
	*getSectSize = sector_size;
}

Bit8u imageDisk::GetBiosType(void) {
	if(!hardDrive) {
		return (Bit8u)DiskGeometryList[floppytype].biosval;
	} else return 0;
}

Bit32u imageDisk::getSectSize(void) {
	return sector_size;
}

static Bit8u GetDosDriveNumber(Bit8u biosNum) {
	switch(biosNum) {
		case 0x0:
			return 0x0;
		case 0x1:
			return 0x1;
		case 0x80:
			return 0x2;
		case 0x81:
			return 0x3;
		case 0x82:
			return 0x4;
		case 0x83:
			return 0x5;
		default:
			return 0x7f;
	}
}

static bool driveInactive(Bit8u driveNum) {
	if(driveNum>=(2 + MAX_HDD_IMAGES)) {
		LOG(LOG_BIOS,LOG_ERROR)("Disk %d non-existant", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	if(imageDiskList[driveNum] == NULL) {
		LOG(LOG_BIOS,LOG_ERROR)("Disk %d not active", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	if(!imageDiskList[driveNum]->active) {
		LOG(LOG_BIOS,LOG_ERROR)("Disk %d not active", driveNum);
		last_status = 0x01;
		CALLBACK_SCF(true);
		return true;
	}
	return false;
}


static Bitu INT13_DiskHandler(void) {
	Bit16u segat, bufptr;
	Bit8u sectbuf[512];
	Bit8u  drivenum;
	Bitu  i,t;
	last_drive = reg_dl;
	drivenum = GetDosDriveNumber(reg_dl);
	bool any_images = false;
	for(i = 0;i < MAX_DISK_IMAGES;i++) {
		if(imageDiskList[i]) any_images=true;
	}

	// unconditionally enable the interrupt flag
	CALLBACK_SIF(true);

	//drivenum = 0;
	//LOG_MSG("INT13: Function %x called on drive %x (dos drive %d)", reg_ah,  reg_dl, drivenum);

	// NOTE: the 0xff error code returned in some cases is questionable; 0x01 seems more correct
	switch(reg_ah) {
	case 0x0: /* Reset disk */
		{
			/* if there aren't any diskimages (so only localdrives and virtual drives)
			 * always succeed on reset disk. If there are diskimages then and only then
			 * do real checks
			 */
			if (any_images && driveInactive(drivenum)) {
				/* driveInactive sets carry flag if the specified drive is not available */
				if ((machine==MCH_CGA) || (machine==MCH_PCJR)) {
					/* those bioses call floppy drive reset for invalid drive values */
					if (((imageDiskList[0]) && (imageDiskList[0]->active)) || ((imageDiskList[1]) && (imageDiskList[1]->active))) {
						if (machine!=MCH_PCJR && reg_dl<0x80) reg_ip++;
						last_status = 0x00;
						CALLBACK_SCF(false);
					}
				}
				return CBRET_NONE;
			}
			if (machine!=MCH_PCJR && reg_dl<0x80) reg_ip++;
			last_status = 0x00;
			CALLBACK_SCF(false);
		}
        break;
	case 0x1: /* Get status of last operation */

		if(last_status != 0x00) {
			reg_ah = last_status;
			CALLBACK_SCF(true);
		} else {
			reg_ah = 0x00;
			CALLBACK_SCF(false);
		}
		break;
	case 0x2: /* Read sectors */
		if (reg_al==0) {
			reg_ah = 0x01;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		if (!any_images) {
			if (drivenum >= DOS_DRIVES || !Drives[drivenum] || Drives[drivenum]->isRemovable()) {
				reg_ah = 0x01;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			// Inherit the Earth cdrom and Amberstar use it as a disk test
			if (((reg_dl&0x80)==0x80) && (reg_dh==0) && ((reg_cl&0x3f)==1)) {
				if (reg_ch==0) {
					PhysPt ptr = PhysMake(SegValue(es),reg_bx);
					// write some MBR data into buffer for Amberstar installer
					mem_writeb(ptr+0x1be,0x80); // first partition is active
					mem_writeb(ptr+0x1c2,0x06); // first partition is FAT16B
				}
				reg_ah = 0;
				CALLBACK_SCF(false);
				return CBRET_NONE;
			}
		}
		if (driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}

		segat = SegValue(es);
		bufptr = reg_bx;
		for(i=0;i<reg_al;i++) {
			last_status = imageDiskList[drivenum]->Read_Sector((Bit32u)reg_dh, (Bit32u)(reg_ch | ((reg_cl & 0xc0)<< 2)), (Bit32u)((reg_cl & 63)+i), sectbuf);
			if((last_status != 0x00) || (killRead)) {
				LOG_MSG("Error in disk read");
				killRead = false;
				reg_ah = 0x04;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			for(t=0;t<512;t++) {
				real_writeb(segat,bufptr,sectbuf[t]);
				bufptr++;
			}
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x3: /* Write sectors */
		
		if(driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
        }                     


		bufptr = reg_bx;
		for(i=0;i<reg_al;i++) {
			for(t=0;t<imageDiskList[drivenum]->getSectSize();t++) {
				sectbuf[t] = real_readb(SegValue(es),bufptr);
				bufptr++;
			}

			last_status = imageDiskList[drivenum]->Write_Sector((Bit32u)reg_dh, (Bit32u)(reg_ch | ((reg_cl & 0xc0) << 2)), (Bit32u)((reg_cl & 63) + i), &sectbuf[0]);
			if(last_status != 0x00) {
            CALLBACK_SCF(true);
				return CBRET_NONE;
			}
        }
		reg_ah = 0x00;
		CALLBACK_SCF(false);
        break;
	case 0x04: /* Verify sectors */
		if (reg_al==0) {
			reg_ah = 0x01;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		if(driveInactive(drivenum)) {
			reg_ah = last_status;
			return CBRET_NONE;
		}

		/* TODO: Finish coding this section */
		/*
		segat = SegValue(es);
		bufptr = reg_bx;
		for(i=0;i<reg_al;i++) {
			last_status = imageDiskList[drivenum]->Read_Sector((Bit32u)reg_dh, (Bit32u)(reg_ch | ((reg_cl & 0xc0)<< 2)), (Bit32u)((reg_cl & 63)+i), sectbuf);
			if(last_status != 0x00) {
				LOG_MSG("Error in disk read");
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			for(t=0;t<512;t++) {
				real_writeb(segat,bufptr,sectbuf[t]);
				bufptr++;
			}
		}*/
		reg_ah = 0x00;
		//Qbix: The following codes don't match my specs. al should be number of sector verified
		//reg_al = 0x10; /* CRC verify failed */
		//reg_al = 0x00; /* CRC verify succeeded */
		CALLBACK_SCF(false);
          
		break;
	case 0x05: /* Format track */
		if (driveInactive(drivenum)) {
			reg_ah = 0xff;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x08: /* Get drive parameters */
		if(driveInactive(drivenum)) {
			last_status = 0x07;
			reg_ah = last_status;
			CALLBACK_SCF(true);
			return CBRET_NONE;
		}
		reg_ax = 0x00;
		reg_bl = imageDiskList[drivenum]->GetBiosType();
		Bit32u tmpheads, tmpcyl, tmpsect, tmpsize;
		imageDiskList[drivenum]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
		if (tmpcyl==0) LOG(LOG_BIOS,LOG_ERROR)("INT13 DrivParm: cylinder count zero!");
		else tmpcyl--;		// cylinder count -> max cylinder
		if (tmpheads==0) LOG(LOG_BIOS,LOG_ERROR)("INT13 DrivParm: head count zero!");
		else tmpheads--;	// head count -> max head
		reg_ch = (Bit8u)(tmpcyl & 0xff);
		reg_cl = (Bit8u)(((tmpcyl >> 2) & 0xc0) | (tmpsect & 0x3f)); 
		reg_dh = (Bit8u)tmpheads;
		last_status = 0x00;
		if (reg_dl&0x80) {	// harddisks
			reg_dl = 0;
			if(imageDiskList[2] != NULL) reg_dl++;
			if(imageDiskList[3] != NULL) reg_dl++;
		} else {		// floppy disks
			reg_dl = 0;
			if(imageDiskList[0] != NULL) reg_dl++;
			if(imageDiskList[1] != NULL) reg_dl++;
		}
		CALLBACK_SCF(false);
		break;
	case 0x11: /* Recalibrate drive */
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	case 0x15: /* Get disk type */
		/* Korean Powerdolls uses this to detect harddrives */
		LOG(LOG_BIOS,LOG_WARN)("INT13: Get disktype used!");
		if (any_images) {
			if(driveInactive(drivenum)) {
				last_status = 0x07;
				reg_ah = last_status;
				CALLBACK_SCF(true);
				return CBRET_NONE;
			}
			Bit32u tmpheads, tmpcyl, tmpsect, tmpsize;
			imageDiskList[drivenum]->Get_Geometry(&tmpheads, &tmpcyl, &tmpsect, &tmpsize);
			Bit64u largesize = tmpheads*tmpcyl*tmpsect*tmpsize;
			largesize/=512;
			Bit32u ts = static_cast<Bit32u>(largesize);
			reg_ah = (drivenum <2)?1:3; //With 2 for floppy MSDOS starts calling int 13 ah 16
			if(reg_ah == 3) {
				reg_cx = static_cast<Bit16u>(ts >>16);
				reg_dx = static_cast<Bit16u>(ts & 0xffff);
			}
			CALLBACK_SCF(false);
		} else {
			if (drivenum <DOS_DRIVES && (Drives[drivenum] != 0 || drivenum <2)) {
				if (drivenum <2) {
					//TODO use actual size (using 1.44 for now).
					reg_ah = 0x1; // type
//					reg_cx = 0;
//					reg_dx = 2880; //Only set size for harddrives.
				} else {
					//TODO use actual size (using 105 mb for now).
					reg_ah = 0x3; // type
					reg_cx = 3;
					reg_dx = 0x4800;
				}
				CALLBACK_SCF(false);
			} else { 
				LOG(LOG_BIOS,LOG_WARN)("INT13: no images, but invalid drive for call 15");
				reg_ah=0xff;
				CALLBACK_SCF(true);
			}
		}
		break;
	case 0x17: /* Set disk type for format */
		/* Pirates! needs this to load */
		killRead = true;
		reg_ah = 0x00;
		CALLBACK_SCF(false);
		break;
	default:
		LOG(LOG_BIOS,LOG_ERROR)("INT13: Function %x called on drive %x (dos drive %d)", reg_ah,  reg_dl, drivenum);
		reg_ah=0xff;
		CALLBACK_SCF(true);
	}
	return CBRET_NONE;
}


void BIOS_SetupDisks(void) {
/* TODO Start the time correctly */
	call_int13=CALLBACK_Allocate();	
	CALLBACK_Setup(call_int13,&INT13_DiskHandler,CB_INT13,"Int 13 Bios disk");
	RealSetVec(0x13,CALLBACK_RealPointer(call_int13));
	int i;
	for(i=0;i<4;i++) {
		imageDiskList[i] = NULL;
	}

	for(i=0;i<MAX_SWAPPABLE_DISKS;i++) {
		diskSwap[i] = NULL;
	}

	diskparm0 = CALLBACK_Allocate();
	diskparm1 = CALLBACK_Allocate();
	swapPosition = 0;

	RealSetVec(0x41,CALLBACK_RealPointer(diskparm0));
	RealSetVec(0x46,CALLBACK_RealPointer(diskparm1));

	PhysPt dp0physaddr=CALLBACK_PhysPointer(diskparm0);
	PhysPt dp1physaddr=CALLBACK_PhysPointer(diskparm1);
	for(i=0;i<16;i++) {
		phys_writeb(dp0physaddr+i,0);
		phys_writeb(dp1physaddr+i,0);
	}

	imgDTASeg = 0;

/* Setup the Bios Area */
	mem_writeb(BIOS_HARDDISK_COUNT,2);

	MAPPER_AddHandler(swapInNextDisk,MK_f4,MMOD1,"swapimg","Swap Image");
	killRead = false;
	swapping_requested = false;
}
