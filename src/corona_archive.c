#include "corona_archive.h"
#include "canopus_services.h"
#include "canopus_memory.h"

/* Original, allocation-bounded ZIP/Deflate reader. The bit decoder implements
 * RFC 1951 canonical prefix codes directly, without a third-party inflater.
 * Archive directory and names remain on disk. Duplicate/overlap checks rescan
 * preceding directory records, trading O(n^2) metadata I/O for constant RAM. */
#define AR_LIMIT (64u * 1024u * 1024u)
#define IO_BYTES 512u
#define BAD (-7)
#define IO (-5)
#define LIMIT (-8)
struct tree { uint16_t count[16], symbol[288]; uint16_t symbols; };
enum stage { EOCD, CENTRAL, EXTRA_C, LOCAL, EXTRA_L, DESCRIPTOR, COMPARE,
             MAKE_PATH, OUTPUT, FINISH_FILE, NEXT_ENTRY, COMPLETE };
struct rh_archive {
    const struct canopus_context_v1 *ctx;
    int32_t source, target;
    int error;
    enum stage stage;
    uint32_t bytes, scan, cd, cd_end, cursor, next, entries, index;
    uint32_t compare, compared, local, data, end, local_sum;
    uint32_t compressed, unpacked, expected_crc, total;
    uint32_t extra, extra_end, made, produced, crc;
    uint16_t flags, method, name_bytes, version;
    uint32_t stamp;
    uint8_t directory, manifest_seen, descriptor, final;
    char root[256], name[256], manifest[256], path[256];
    uint8_t scratch[IO_BYTES + 24u];
    uint8_t input[IO_BYTES], output[IO_BYTES], history[32768];
    uint32_t input_at, input_n, input_pos, input_end;
    uint32_t output_n, output_at;
    uint32_t bits; unsigned bit_count;
    uint32_t stored, match, distance;
    uint8_t inflate_state;
    struct tree literal, dist, codes;
};
static uint16_t le16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p+2)<<16; }
static unsigned length(const char *s) { unsigned n=0; while(s[n]) n++; return n; }
static int same(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; } return *a==*b; }
static void clear(void *p,uint32_t n) { uint8_t *b=p; while(n--) *b++=0; }
static int latch(struct rh_archive *a,int rc) { a->error=rc; return rc; }
static int read_at(struct rh_archive *a,uint32_t at,void *p,uint32_t n) {
    uint32_t done=0;
    if(at>a->bytes || n>a->bytes-at) return BAD;
    if(a->ctx->fs->seek(a->ctx,a->source,at,CANOPUS_FS_SEEK_SET)!=(int64_t)at) return IO;
    while(done<n) {
        int32_t r=a->ctx->fs->read(a->ctx,a->source,(uint8_t *)p+done,n-done);
        if(r<=0 || (uint32_t)r>n-done) return IO;
        done+=(uint32_t)r;
    }
    return 0;
}
static int full_path(struct rh_archive *a,const char *name,char out[256]) {
    unsigned n=length(a->root),m=length(name);
    if(n+1u+m>255u) return LIMIT;
    canopus_memcpy(out,a->root,n); out[n++]='/';canopus_memcpy(out+n,name,m+1u);return 0;
}
static int valid_utf8(const uint8_t *s,unsigned n) {
    unsigned i=0;
    while(i<n) {
        unsigned c=s[i++],k,min,v;
        if(c<128u) continue;
        if(c>=194u && c<=223u) {k=1;min=128u;v=c&31u;}
        else if(c>=224u && c<=239u) {k=2;min=2048u;v=c&15u;}
        else if(c>=240u && c<=244u) {k=3;min=65536u;v=c&7u;}
        else return 0;
        if(k>n-i) return 0;
        while(k--) {c=s[i++];if((c&192u)!=128u)return 0;v=v*64u+(c&63u);}
        if(v<min || v>0x10ffffu || (v>=0xd800u && v<=0xdfffu)) return 0;
    }
    return 1;
}
static int valid_name(const char *name,unsigned n) {
    unsigned start=0;
    if(!n || name[0]=='/' || !valid_utf8((const uint8_t *)name,n)) return 0;
    for(unsigned i=0;i<=n;i++) {
        unsigned c=(uint8_t)name[i];
        if(c=='\\' || c==':' || (i<n && (c<32u || c==127u))) return 0;
        if(c=='/' || i==n) {
            unsigned size=i-start;
            if(size>CANOPUS_FS_NAME_MAX)return LIMIT;
            if(!size && !(i==n && i && name[i-1u]=='/')) return 0;
            if((size==1u && name[start]=='.') || (size==2u && name[start]=='.' && name[start+1u]=='.')) return 0;
            if(size==12u) {static const char forbidden[]="mappings.tsv";unsigned j=0;while(j<size && name[start+j]==forbidden[j])j++;if(j==size)return 0;}
            start=i+1u;
        }
    }
    return 1;
}
static int extra_step(struct rh_archive *a,enum stage after) {
    int rc;
    if(a->extra==a->extra_end) {a->stage=after;return 0;}
    if(a->extra_end-a->extra<4u)return BAD;
    rc=read_at(a,a->extra,a->scratch,4);if(rc)return rc;
    uint32_t n=le16(a->scratch+2);
    if(n>a->extra_end-a->extra-4u || le16(a->scratch)==1u || le16(a->scratch)==0x9901u)return BAD;
    a->extra+=4u+n;return 0;
}
static int tree_build(struct tree *t,const uint8_t *sizes,unsigned n,int kind) {
    uint16_t pos[16]; int available=1; unsigned nonzero=0,max=0;
    clear(t,sizeof(*t));
    for(unsigned i=0;i<n;i++) {if(sizes[i]>15u)return BAD;t->count[sizes[i]]++;if(sizes[i]){nonzero++;if(sizes[i]>max)max=sizes[i];}}
    if(!nonzero) return kind==2 ? 0:BAD;
    for(unsigned i=1;i<=15;i++) {available=2*available-t->count[i];if(available<0)return BAD;}
    /* Code-length alphabets must be complete. Literal/distance alphabets may
     * consist of a single one-bit code; other incomplete trees are invalid. */
    if(available && (kind==0 || max!=1u)) return BAD;
    pos[1]=0;for(unsigned i=2;i<=15;i++)pos[i]=(uint16_t)(pos[i-1]+t->count[i-1]);
    for(unsigned i=0;i<n;i++)if(sizes[i])t->symbol[pos[sizes[i]]++]=(uint16_t)i;
    t->symbols=(uint16_t)nonzero;return 0;
}
static int take_bits(struct rh_archive *a,unsigned n,uint32_t *v) {
    while(a->bit_count<n) {
        if(a->input_at==a->input_n) {
            uint32_t remaining=a->input_end-a->input_pos;
            int rc;
            if(!remaining)return BAD;
            a->input_n=remaining>IO_BYTES?IO_BYTES:remaining;
            rc=read_at(a,a->input_pos,a->input,a->input_n);if(rc)return rc;
            a->input_pos+=a->input_n;a->input_at=0;
        }
        a->bits|=(uint32_t)a->input[a->input_at++]<<a->bit_count;a->bit_count+=8u;
    }
    *v=a->bits & ((1u<<n)-1u);a->bits>>=n;a->bit_count-=n;return 0;
}
static int decode(struct rh_archive *a,const struct tree *t,uint32_t *symbol) {
    uint32_t code=0,first=0,index=0,bit;int rc;
    if(!t->symbols)return BAD;
    for(unsigned width=1;width<=15;width++) {
        rc=take_bits(a,1,&bit);if(rc)return rc;
        code=(code<<1)|bit;
        if(code>=first && code-first<t->count[width]) {*symbol=t->symbol[index+code-first];return 0;}
        index+=t->count[width];first=(first+t->count[width])<<1;
    }
    return BAD;
}
static int dynamic_trees(struct rh_archive *a) {
    static const uint8_t permutation[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t sizes[320],cl[19];uint32_t nl,nd,nc,v;int rc;
    clear(cl,sizeof(cl));
    rc=take_bits(a,5,&nl);if(rc)return rc;nl+=257u;
    rc=take_bits(a,5,&nd);if(rc)return rc;nd++;
    rc=take_bits(a,4,&nc);if(rc)return rc;nc+=4u;
    if(nl>286u || nd>32u)return BAD;
    for(unsigned i=0;i<nc;i++){rc=take_bits(a,3,&v);if(rc)return rc;cl[permutation[i]]=(uint8_t)v;}
    rc=tree_build(&a->codes,cl,19,0);if(rc)return rc;
    for(unsigned i=0;i<nl+nd;) {
        rc=decode(a,&a->codes,&v);if(rc)return rc;
        if(v<=15u) {sizes[i++]=(uint8_t)v;continue;}
        unsigned repeat;uint8_t value=0;
        if(v==16u){if(!i)return BAD;value=sizes[i-1u];rc=take_bits(a,2,&v);repeat=v+3u;}
        else if(v==17u){rc=take_bits(a,3,&v);repeat=v+3u;}
        else if(v==18u){rc=take_bits(a,7,&v);repeat=v+11u;}
        else return BAD;
        if(rc)return rc;if(repeat>nl+nd-i)return BAD;
        while(repeat--)sizes[i++]=value;
    }
    if(!sizes[256])return BAD;
    rc=tree_build(&a->literal,sizes,nl,1);if(rc)return rc;
    return tree_build(&a->dist,sizes+nl,nd,2);
}
static int fixed_trees(struct rh_archive *a) {
    uint8_t sizes[288];int rc;
    for(unsigned i=0;i<288;i++)sizes[i]=(uint8_t)(i<144u?8:i<256u?9:i<280u?7:8);
    rc=tree_build(&a->literal,sizes,288,1);if(rc)return rc;
    for(unsigned i=0;i<32;i++)sizes[i]=5;
    return tree_build(&a->dist,sizes,32,2);
}
static int emit(struct rh_archive *a,uint8_t byte) {
    if(a->produced>=a->unpacked || a->total>=AR_LIMIT)return a->total>=AR_LIMIT?LIMIT:BAD;
    a->history[a->produced&32767u]=byte;a->produced++;a->total++;
    a->output[a->output_n++]=byte;
    a->crc^=byte;for(unsigned k=0;k<8;k++)a->crc=(a->crc>>1)^((0u-(a->crc&1u))&0xedb88320u);
    return 0;
}
/* One call emits at most IO_BYTES. A no-output Deflate block ends this call,
 * preventing arbitrarily long runs of empty blocks from monopolizing owner. */
static int inflate_step(struct rh_archive *a) {
    static const uint16_t base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    /* Keep distance bases word-sized: adjacent halfwords can otherwise look
     * like absolute firmware addresses to the native payload verifier. */
    static const uint32_t dbase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    uint32_t v,w;int rc;
    while(a->output_n<IO_BYTES) {
        if(a->inflate_state==0) {
            rc=take_bits(a,1,&v);if(rc)return rc;a->final=(uint8_t)v;
            rc=take_bits(a,2,&v);if(rc)return rc;
            if(!v) {
                a->bits=0;a->bit_count=0;
                rc=take_bits(a,16,&v);if(rc)return rc;rc=take_bits(a,16,&w);if(rc)return rc;
                if((v^w)!=65535u)return BAD;a->stored=v;a->inflate_state=1;
            } else if(v==1u) {rc=fixed_trees(a);if(rc)return rc;a->inflate_state=2;}
            else if(v==2u) {rc=dynamic_trees(a);if(rc)return rc;a->inflate_state=2;}
            else return BAD;
        }
        if(a->inflate_state==1) {
            if(!a->stored) {a->inflate_state=a->final?4:0;return 0;}
            rc=take_bits(a,8,&v);if(rc)return rc;rc=emit(a,(uint8_t)v);if(rc)return rc;a->stored--;
        } else if(a->inflate_state==2) {
            rc=decode(a,&a->literal,&v);if(rc)return rc;
            if(v<256u) {rc=emit(a,(uint8_t)v);if(rc)return rc;}
            else if(v==256u) {a->inflate_state=a->final?4:0;return 0;}
            else {
                if(v>285u)return BAD;v-=257u;
                rc=take_bits(a,extra[v],&w);if(rc)return rc;a->match=base[v]+w;
                rc=decode(a,&a->dist,&v);if(rc)return rc;if(v>=30u)return BAD;
                unsigned n=v<4u?0u:(unsigned)(v/2u-1u);
                rc=take_bits(a,n,&w);if(rc)return rc;a->distance=dbase[v]+w;
                if(a->distance>a->produced || a->match>a->unpacked-a->produced)return BAD;
                a->inflate_state=3;
            }
        } else if(a->inflate_state==3) {
            rc=emit(a,a->history[(a->produced-a->distance)&32767u]);if(rc)return rc;
            if(!--a->match)a->inflate_state=2;
        } else if(a->inflate_state==4) return 0;
    }
    return 0;
}
static int eocd_step(struct rh_archive *a) {
    uint32_t start=a->scan>IO_BYTES?a->scan-IO_BYTES:0u;
    uint32_t lower=a->bytes>65557u?a->bytes-65557u:0u;
    int rc;
    if(start<lower)start=lower;
    rc=read_at(a,start,a->scratch,a->scan-start+22u);if(rc)return rc;
    uint32_t at=a->scan-start;
    for(;;) {
        const uint8_t *p=a->scratch+at;
        if(le32(p)==0x06054b50u && le16(p+20)==a->bytes-start-at-22u) {
            uint32_t end=start+at,entries=le16(p+10),cd=le32(p+16),size=le32(p+12);
            /* Comments may contain EOCD signatures. Only commit parser state
             * once a candidate describes a plausible single-volume directory;
             * an invalid candidate does not hide the real preceding EOCD. */
            if(!le16(p+4) && !le16(p+6) && le16(p+8)==entries &&
               entries && entries!=65535u && cd<=end && size==end-cd &&
               size>=entries*47u) {
                a->entries=entries;a->cd=cd;
                a->cd_end=end;a->cursor=cd;a->stage=CENTRAL;return 0;
            }
        }
        if(!at)break;at--;
    }
    if(start==lower)return BAD;
    a->scan=start-1u;return 0;
}
static int central_step(struct rh_archive *a) {
    int rc;uint8_t *p=a->scratch;
    if(a->index==a->entries) {
        if(a->cursor!=a->cd_end || a->local_sum!=a->cd || !a->manifest_seen)return BAD;
        a->stage=COMPLETE;return 1;
    }
    if(a->cursor>a->cd_end || a->cd_end-a->cursor<46u)return BAD;
    rc=read_at(a,a->cursor,p,46u);if(rc)return rc;
    if(le32(p)!=0x02014b50u || le16(p+6)<10u || le16(p+6)>20u || le16(p+34))return BAD;
    a->version=le16(p+6);a->stamp=le32(p+12);
    a->flags=le16(p+8);a->method=le16(p+10);
    if((a->flags&~0x080eu) || (a->method!=0u && a->method!=8u) || (!a->method && (a->flags&6u)) || (a->method==8u && a->version<20u))return BAD;
    a->expected_crc=le32(p+16);a->compressed=le32(p+20);a->unpacked=le32(p+24);
    a->name_bytes=le16(p+28);a->local=le32(p+42);
    if(!a->name_bytes || a->name_bytes>255u)return BAD;
    uint32_t remain=a->cd_end-a->cursor;
    uint32_t n=46u+a->name_bytes+le16(p+30)+le16(p+32);
    if(n>remain || a->compressed==UINT32_MAX || a->unpacked==UINT32_MAX || a->local>=a->cd)return BAD;
    if(a->unpacked>AR_LIMIT-a->total)return LIMIT;
    unsigned mode=le32(p+38)>>16;
    a->next=a->cursor+n;a->extra=a->cursor+46u+a->name_bytes;a->extra_end=a->extra+le16(p+30);
    uint32_t attrs=le32(p+38);
    rc=read_at(a,a->cursor+46u,a->name,a->name_bytes);if(rc)return rc;a->name[a->name_bytes]=0;
    rc=valid_name(a->name,a->name_bytes);if(rc!=1)return rc<0?rc:BAD;
    a->directory=(uint8_t)(a->name[a->name_bytes-1u]=='/');
    /* Unix file-type bits are authoritative where present; reject symlinks,
     * devices and other special entries even when the filename looks benign. */
    if((mode&0170000u) && (mode&0170000u)!=(a->directory?0040000u:0100000u))return BAD;
    if((attrs&8u) || ((attrs&0x10u) && !a->directory))return BAD;
    if(a->directory && (a->unpacked || a->expected_crc || (!a->method && a->compressed)))return BAD;
    rc=full_path(a,a->name,a->path);if(rc)return rc;
    if(same(a->name,"corona.json") || same(a->name,"canora.json")) {
        if(a->manifest_seen || a->directory)return BAD;
        if(a->unpacked>65536u)return LIMIT;
        a->manifest_seen=1;canopus_memcpy(a->manifest,a->path,length(a->path)+1u);
    }
    a->stage=EXTRA_C;return 0;
}
static int local_step(struct rh_archive *a) {
    uint8_t *p=a->scratch;int rc;
    if(a->local>a->cd || a->cd-a->local<30u)return BAD;
    rc=read_at(a,a->local,p,30u);if(rc)return rc;
    if(le32(p)!=0x04034b50u || le16(p+4)!=a->version || le32(p+10)!=a->stamp || le16(p+6)!=a->flags || le16(p+8)!=a->method || le16(p+26)!=a->name_bytes)return BAD;
    if(a->flags&8u) {
        if((le32(p+14) && le32(p+14)!=a->expected_crc) || (le32(p+18) && le32(p+18)!=a->compressed) || (le32(p+22) && le32(p+22)!=a->unpacked))return BAD;
    } else if(le32(p+14)!=a->expected_crc || le32(p+18)!=a->compressed || le32(p+22)!=a->unpacked)return BAD;
    uint32_t extra=le16(p+28), header=30u+a->name_bytes+extra;
    if(header>a->cd-a->local || a->compressed>a->cd-a->local-header)return BAD;
    a->data=a->local+header;a->end=a->data+a->compressed;
    a->extra=a->local+30u+a->name_bytes;a->extra_end=a->extra+extra;
    rc=read_at(a,a->local+30u,p,a->name_bytes);if(rc)return rc;
    for(unsigned i=0;i<a->name_bytes;i++)if(p[i]!=(uint8_t)a->name[i])return BAD;
    a->stage=EXTRA_L;return 0;
}
static int descriptor_step(struct rh_archive *a) {
    int rc;
    if(a->flags&8u) {
        if(a->cd-a->end<12u)return BAD;
        uint32_t count=a->cd-a->end>=16u?16u:12u;
        rc=read_at(a,a->end,a->scratch,count);if(rc)return rc;
        uint8_t *p=a->scratch;
        if(count==16u && le32(p)==0x08074b50u && le32(p+4)==a->expected_crc && le32(p+8)==a->compressed && le32(p+12)==a->unpacked)a->end+=16u;
        else if(le32(p)==a->expected_crc && le32(p+4)==a->compressed && le32(p+8)==a->unpacked)a->end+=12u;
        else return BAD;
    }
    a->compare=a->cd;a->compared=0;a->stage=COMPARE;return 0;
}
static int compare_step(struct rh_archive *a) {
    int rc;uint8_t *p=a->scratch;
    if(a->compared==a->index) {
        if(a->end-a->local>a->cd-a->local_sum)return BAD;
        a->local_sum+=a->end-a->local;a->made=length(a->root)+1u;a->stage=MAKE_PATH;return 0;
    }
    rc=read_at(a,a->compare,p,46u);if(rc)return rc;
    uint32_t n=le16(p+28),prior_local=le32(p+42),compressed=le32(p+20);
    unsigned flags=le16(p+8);uint32_t next=a->compare+46u+n+le16(p+30)+le16(p+32);
    rc=read_at(a,a->compare+46u,p,n);if(rc)return rc;p[n]=0;
    unsigned prior_dir=p[n-1u]=='/',len=(unsigned)n-prior_dir,cur=a->name_bytes-a->directory,common=len<cur?len:cur,i=0;
    while(i<common && p[i]==(uint8_t)a->name[i])i++;
    if(i==common) {
        if(len==cur)return BAD;
        if(len<cur && a->name[len]=='/' && !prior_dir)return BAD;
        if(cur<len && p[cur]=='/' && !a->directory)return BAD;
    }
    rc=read_at(a,prior_local,p,30u);if(rc)return rc;
    uint32_t prior_end=prior_local+30u+le16(p+26)+le16(p+28)+compressed;
    if(flags&8u) {
        rc=read_at(a,prior_end,p,4);if(rc)return rc;
        /* Prior descriptor was validated. Its signature can equal the CRC;
         * disambiguate by testing the following three expected fields. */
        if(le32(p)==0x08074b50u && a->cd-prior_end>=16u) {
            uint8_t h[16];rc=read_at(a,prior_end,h,16);if(rc)return rc;
            uint8_t prior[12];rc=read_at(a,a->compare+16u,prior,12);if(rc)return rc;
            prior_end+=(le32(h+4)==le32(prior) && le32(h+8)==le32(prior+4) && le32(h+12)==le32(prior+8))?16u:12u;
        } else prior_end+=12u;
    }
    if(a->local<prior_end && prior_local<a->end)return BAD;
    a->compare=next;a->compared++;return 0;
}
static int mkdir_path_step(struct rh_archive *a) {
    unsigned n=length(a->path);int rc;struct canopus_fs_stat st;
    for(;a->made<n;a->made++) {
        if(a->path[a->made]!='/')continue;
        a->path[a->made]=0;
        rc=a->ctx->fs->exists(a->ctx,a->path);
        if(rc==0)rc=a->ctx->fs->mkdir(a->ctx,a->path);
        else if(rc>0){rc=a->ctx->fs->stat(a->ctx,a->path,&st);if(!rc && !st.is_dir)rc=BAD;}
        a->path[a->made++]='/';return rc<0?(rc==BAD?BAD:IO):0;
    }
    if(!a->directory) {
        rc=a->ctx->fs->exists(a->ctx,a->path);if(rc)return rc<0?IO:BAD;
        a->target=a->ctx->fs->open(a->ctx,a->path,CANOPUS_FS_WRITE);
        if(a->target<0)return IO;
    }
    a->input_pos=a->data;a->input_end=a->data+a->compressed;a->input_at=a->input_n=0;
    a->output_n=a->output_at=a->produced=a->bits=a->bit_count=0;
    a->stored=a->match=a->distance=a->inflate_state=0;a->crc=UINT32_MAX;
    a->stage=OUTPUT;return 0;
}
static int output_step(struct rh_archive *a) {
    int rc;
    if(a->output_at<a->output_n) {
        int32_t n=a->ctx->fs->write(a->ctx,a->target,a->output+a->output_at,a->output_n-a->output_at);
        if(n<=0 || (uint32_t)n>a->output_n-a->output_at)return IO;
        a->output_at+=(uint32_t)n;return 0;
    }
    a->output_at=a->output_n=0;
    if(a->method==0u) {
        if(a->compressed!=a->unpacked)return BAD;
        if(a->produced==a->unpacked) {a->stage=FINISH_FILE;return 0;}
        uint32_t n=a->unpacked-a->produced;if(n>IO_BYTES)n=IO_BYTES;
        rc=read_at(a,a->data+a->produced,a->input,n);if(rc)return rc;
        for(unsigned i=0;i<n;i++){rc=emit(a,a->input[i]);if(rc)return rc;}
        return 0;
    }
    rc=inflate_step(a);if(rc)return rc;
    if(a->inflate_state==4 && !a->output_n) {
        if(a->input_pos-a->input_n+a->input_at!=a->input_end || a->produced!=a->unpacked)return BAD;
        a->stage=FINISH_FILE;
    }
    return 0;
}
int rh_archive_create(const struct canopus_context_v1 *ctx,const char *archive_path,
                      uint32_t archive_bytes,const char *staging_path,struct rh_archive **out) {
    struct rh_archive *a;int rc;
    if(!out)return BAD;*out=0;
    if(!ctx || !ctx->fs || !ctx->mem || !ctx->mem->alloc || !ctx->mem->free || !ctx->fs->open || !ctx->fs->read || !ctx->fs->write || !ctx->fs->seek || !ctx->fs->size || !ctx->fs->sync || !ctx->fs->close || !ctx->fs->exists || !ctx->fs->stat || !ctx->fs->mkdir)return -11;
    if(!archive_path || !staging_path || !staging_path[0] || length(staging_path)>255u || staging_path[length(staging_path)-1u]=='/' || archive_bytes<22u)return BAD;
    if(archive_bytes>AR_LIMIT)return LIMIT;
    a=ctx->mem->alloc(ctx,(uint32_t)sizeof(*a));if(!a)return LIMIT;
    clear(a,sizeof(*a));a->ctx=ctx;a->source=a->target=-1;a->bytes=archive_bytes;a->scan=archive_bytes-22u;
    if(staging_path[0]=='/')canopus_memcpy(a->root,staging_path,length(staging_path)+1u);
    else {
        if(!ctx->fs->get_data_path){rh_archive_destroy(a);return -11;}
        rc=ctx->fs->get_data_path(ctx,a->root,sizeof(a->root));
        if(rc<0){rh_archive_destroy(a);return IO;}
        unsigned n=0,m=length(staging_path);
        while(n<sizeof(a->root) && a->root[n])n++;
        if(!n || n==sizeof(a->root)){rh_archive_destroy(a);return BAD;}
        if(a->root[n-1u]=='/')n--;
        if(n+1u+m>255u){rh_archive_destroy(a);return LIMIT;}
        a->root[n++]='/';canopus_memcpy(a->root+n,staging_path,m+1u);
    }
    a->source=ctx->fs->open(ctx,archive_path,CANOPUS_FS_READ);
    if(a->source<0){rh_archive_destroy(a);return IO;}
    int64_t source_bytes=ctx->fs->size(ctx,a->source);
    if(source_bytes<0){rh_archive_destroy(a);return IO;}
    if(source_bytes<archive_bytes){rh_archive_destroy(a);return BAD;}
    rc=ctx->fs->exists(ctx,staging_path);
    if(rc || ctx->fs->mkdir(ctx,staging_path)<0) {rh_archive_destroy(a);return rc>0?BAD:IO;}
    *out=a;return 0;
}
int rh_archive_step(struct rh_archive *a) {
    int rc=0;
    if(!a)return BAD;if(a->error)return a->error;
    switch(a->stage) {
    case EOCD:rc=eocd_step(a);break;
    case CENTRAL:rc=central_step(a);break;
    case EXTRA_C:rc=extra_step(a,LOCAL);break;
    case LOCAL:rc=local_step(a);break;
    case EXTRA_L:rc=extra_step(a,DESCRIPTOR);break;
    case DESCRIPTOR:rc=descriptor_step(a);break;
    case COMPARE:rc=compare_step(a);break;
    case MAKE_PATH:rc=mkdir_path_step(a);break;
    case OUTPUT:rc=output_step(a);break;
    case FINISH_FILE:
        if(a->produced!=a->unpacked || (a->crc^UINT32_MAX)!=a->expected_crc)rc=BAD;
        else if(a->target<0)a->stage=NEXT_ENTRY;
        else if(a->ctx->fs->sync(a->ctx,a->target)<0)rc=IO;
        else {int32_t fd=a->target;a->target=-1;if(a->ctx->fs->close(a->ctx,fd)<0)rc=IO;else a->stage=NEXT_ENTRY;}
        break;
    case NEXT_ENTRY:a->cursor=a->next;a->index++;a->stage=CENTRAL;break;
    case COMPLETE:return 1;
    }
    return rc<0?latch(a,rc):rc;
}
void rh_archive_destroy(struct rh_archive *a) {
    if(!a)return;
    if(a->target>=0)(void)a->ctx->fs->close(a->ctx,a->target);
    if(a->source>=0)(void)a->ctx->fs->close(a->ctx,a->source);
    a->ctx->mem->free(a->ctx,a);
}
int rh_archive_manifest(struct rh_archive *a,char out[256]) {
    if(!a || !out || a->error || a->stage!=COMPLETE)return BAD;
    canopus_memcpy(out,a->manifest,length(a->manifest)+1u);return 0;
}
