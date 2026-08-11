// Validate the emitted SD fixtures through the DEVICE's own verify code.
// Opens a testnet session on the dev seed, then loads each file with the
// matching script type and checks the expected verdict and caution flags.
//
// This exists so a fixture cannot go stale in silence. A file written to an SD
// card is checked by a person holding the device, and a person holding the
// device cannot tell a fixture that stopped reaching the screen it was built
// for from a signer that stopped flagging what it should.
#include <stdio.h>
#include <string.h>
#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "kiss_seed.h"

static const char *st(int s){ return s==WPSBT_READY?"READY":s==WPSBT_CAUTION?"CAUTION":"STOP"; }

static size_t slurp(const char *dir, const char *name, unsigned char *buf, size_t cap){
    char path[1024]; snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE *fp=fopen(path,"rb"); if(!fp) return 0;
    size_t n=fread(buf,1,cap,fp); fclose(fp); return n;
}

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: %s <dir>\n",argv[0]); return 2; }
    kiss_seed_store("abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about");
    kiss_set_network(1);                         // testnet
    if(kiss_session_open("")!=0){ printf("session open failed\n"); return 1; }
    // want_flags is checked EXACTLY, not as a subset: a fixture built for one
    // row on the screen that quietly starts raising two is a fixture that no
    // longer shows what the tester was told to look at.
    struct { const char *f; int sc; int want; uint16_t flags; } t[] = {
        {"1-native.psbt",            WSCRIPT_NATIVE, WPSBT_READY,   0},
        {"2-nested.psbt",            WSCRIPT_NESTED, WPSBT_READY,   0},
        {"3-legacy.psbt",            WSCRIPT_LEGACY, WPSBT_READY,   0},
        {"4-stop-wrongnet.psbt",     WSCRIPT_NATIVE, WPSBT_STOP,    0},
        {"5-caution-highfee.psbt",   WSCRIPT_NATIVE, WPSBT_CAUTION, WPSBT_C_HIGHFEE},
        {"6-unproven-2in.psbt",      WSCRIPT_NATIVE, WPSBT_CAUTION, WPSBT_C_UNPROVEN_IN},
        {"7-proven-2in.psbt",        WSCRIPT_NATIVE, WPSBT_READY,   0},
        {"8-stop-contradiction.psbt",WSCRIPT_NATIVE, WPSBT_STOP,    0},
        {"9-caution-five.psbt",      WSCRIPT_NATIVE, WPSBT_CAUTION,
             WPSBT_C_UNPROVEN_IN | WPSBT_C_HIGHFEE | WPSBT_C_DUST_INPUT |
             WPSBT_C_MERGE_INS   | WPSBT_C_DUST_CHANGE},
        // READY and no flags is the POINT of this one. It exists to fill the
        // recipient list past the fold, and what is being looked at on device is
        // whether HOLD TO SIGN stays inert until that list has been read to its
        // end. A caution row here would put a second gate in front of the first
        // and neither would be the thing under test.
        {"10-many-recipients.psbt",  WSCRIPT_NATIVE, WPSBT_READY,   0},
    };
    int fails=0;
    // sizeof, not a literal 4. The bound was hardcoded, so adding a fixture to
    // the table above compiled clean and quietly checked everything except the
    // new one, which is the failure mode a fixture list can least afford.
    for(size_t i=0;i<sizeof t/sizeof *t;i++){
        unsigned char buf[8192];
        size_t n=slurp(argv[1],t[i].f,buf,sizeof buf);
        if(!n){ printf("FAIL missing %s\n",t[i].f); fails++; continue; }
        kiss_set_script(t[i].sc);
        wpsbt_summary_t sum; memset(&sum,0,sizeof sum);
        int rc=kiss_psbt_load(buf,n,&sum);
        int ok=(rc==0 && sum.status==t[i].want && sum.caution_flags==t[i].flags);
        printf("%s %-26s rc=%d status=%-7s flags=0x%02X (want %s 0x%02X)\n",
               ok?"PASS":"FAIL", t[i].f, rc, st(sum.status),
               sum.caution_flags, st(t[i].want), t[i].flags);
        if(!ok) fails++;
        kiss_psbt_free();
    }

    // THE pair. 6 and 7 spend the same two coins in the same transaction and
    // differ only in whether the previous transactions ride along. Reading the
    // amount off the previous transaction instead of the witness_utxo must not
    // move a byte of the signature, or this signer stops agreeing with every
    // other one it ever co-signed with. On the device this is the SIGNATURE
    // CHECK code on the signed screen: it must read the same for both files.
    {
        unsigned char pb[8192], sb[8192];
        char fa[9]="", fb[9]="";
        const char *pair[2]={"6-unproven-2in.psbt","7-proven-2in.psbt"};
        char *out[2]={fa,fb};
        int ok=1;
        kiss_set_script(WSCRIPT_NATIVE);
        for(int i=0;i<2;i++){
            size_t n=slurp(argv[1],pair[i],pb,sizeof pb), w=0;
            wpsbt_summary_t sum; memset(&sum,0,sizeof sum);
            if(!n || kiss_psbt_load(pb,n,&sum)!=0 ||
               kiss_psbt_sign(sb,sizeof sb,&w)!=0 ||
               kiss_psbt_sig_fingerprint(sb,w,out[i])!=0) ok=0;
            kiss_psbt_free();
        }
        ok = ok && strcmp(fa,fb)==0;
        printf("%s proof does not change the signature   %s vs %s\n",
               ok?"PASS":"FAIL", fa[0]?fa:"?", fb[0]?fb:"?");
        if(!ok) fails++;
        printf("     on device: SIGNATURE CHECK must read %s for both\n",
               fa[0]?fa:"?");
    }

    kiss_session_close();
    printf(fails?"\n%d FAIL\n":"\nALL PASS\n", fails);
    return fails?1:0;
}
