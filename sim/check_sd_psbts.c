// Validate the emitted SD fixtures through the DEVICE's own verify code.
// Opens a testnet session on the dev seed, then loads each file with the
// matching script type and checks the expected verdict.
#include <stdio.h>
#include <string.h>
#include "wallet_crypto.h"
#include "wallet_psbt.h"
#include "wallet_seed.h"

static const char *st(int s){ return s==WPSBT_READY?"READY":s==WPSBT_CAUTION?"CAUTION":"STOP"; }

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: %s <dir>\n",argv[0]); return 2; }
    wallet_seed_store("abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about");
    wallet_set_network(1);                         // testnet
    if(wallet_session_open("")!=0){ printf("session open failed\n"); return 1; }
    struct { const char *f; int sc; int want; } t[] = {
        {"1-native.psbt",        WSCRIPT_NATIVE, WPSBT_READY},
        {"2-nested.psbt",        WSCRIPT_NESTED, WPSBT_READY},
        {"3-legacy.psbt",        WSCRIPT_LEGACY, WPSBT_READY},
        {"4-stop-wrongnet.psbt", WSCRIPT_NATIVE, WPSBT_STOP},
    };
    int fails=0;
    for(int i=0;i<4;i++){
        char path[1024]; snprintf(path,sizeof path,"%s/%s",argv[1],t[i].f);
        FILE *fp=fopen(path,"rb"); if(!fp){ printf("FAIL missing %s\n",t[i].f); fails++; continue; }
        unsigned char buf[8192]; size_t n=fread(buf,1,sizeof buf,fp); fclose(fp);
        wallet_set_script(t[i].sc);
        wpsbt_summary_t sum; memset(&sum,0,sizeof sum);
        int rc=wallet_psbt_load(buf,n,&sum);
        int ok=(rc==0 && sum.status==t[i].want);
        printf("%s %-22s rc=%d status=%s (want %s)\n",
               ok?"PASS":"FAIL", t[i].f, rc, st(sum.status), st(t[i].want));
        if(!ok) fails++;
    }
    wallet_session_close();
    printf(fails?"\n%d FAIL\n":"\nALL PASS\n", fails);
    return fails?1:0;
}
