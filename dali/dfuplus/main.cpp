/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#pragma warning (disable : 4786)
#include <build-config.h>
#include "daftcfg.hpp"
#include "dfuerror.hpp"
#include "dfuplus.hpp"
#if defined( __linux__) || defined(__FreeBSD__)
#include "termios.h"
#endif

void printVersion()
{
    printf("DFU Version: %d %s\n", DAFT_VERSION, BUILD_TAG);
}

void handleSyntax()
{
    StringBuffer out;

    out.append("Usage:\n");
    out.append("    dfuplus action=[spray|replicate|despray|copy|remove|rename|list|\n");
    out.append("                    addsuper|removesuper|listsuper|copysuper|dafilesrv|\n");
    out.append("                    savexml|add|status|abort|resubmit|monitor] {<options>}\n");
    out.append("    general options:\n");
    out.append("        server=<esp-server-url> \n");
    out.append("        username=<user-name>\n");
    out.append("        password=<password>\n");
    out.append("        overwrite=0|1\n");
    out.append("        replicate=1|0\n");
    out.append("        replicateoffset=N -- node relative offset to find replicate (default 1)\n");
    out.append("        partlist=<part-list> -- for one or more parts   \n");
    out.append("        @filename -- read options from filename \n");
    out.append("        nowait=0|1 -- return immediately without waiting for completion.\n");
    out.append("        connect=<nn> -- restrict to nn connections at a time.\n");
    out.append("        transferbuffersize=<n> -- use buffer of size n bytes when transferring data.\n");
    out.append("        throttle=<nnn> -- restrict the entire transfer speed to nnn Mbits/second\n");
    out.append("        norecover=0|1 -- don't create or restore from recovery information\n");
    out.append("        nosplit=0|1 -- optional, don't split a file part to multiple target parts\n");
    out.append("        compress=0|1 -- optional, compress target\n");
    out.append("        encrypt=<password> -- optional, encrypt target\n");
    out.append("        decrypt=<password> -- optional, decrypt source\n");
    out.append("        push=0|1 -- optional override pull/push default\n");
    out.append("        jobname=<jobname> -- specify the jobname for spray, despray, copy, rename and replicate.\n");
    out.append("    spray options:\n");
    out.append("        srcip=<source-machine-ip>\n");
    out.append("        srcfile=<source-file-path>\n");
    out.append("        srcxml=<xml-file> -- replaces srcip and srcfile\n");
    out.append("        dstname=<destination-logical-name>\n");
    out.append("        dstcluster=<cluster-name>\n");
    out.append("        format=fixed|csv|xml|variable|recfmv|recfmvb\n");
    out.append("        prefix=filename{:length},filesize{:[B|L][1-8]}\n");
    out.append("        options for fixed:\n");
    out.append("            recordsize=<record-size>\n");
    out.append("        options for csv:\n");
    out.append("            encoding=ascii|utf8|utf8n|utf16|utf16le|utf16be|utf32|utf32le|utf32be -- optional, default is ascii\n");
    out.append("            maxrecordsize=<max-record-size> -- optional, default is 8192\n");
    out.append("            separator=<separator> -- optional, default is \\,\n");
    out.append("            terminator=<terminator> -- optional, default is \\r,\\r\\n\n");
    out.append("            quote=<quote> -- optional, default is '\n");
    out.append("        options for xml:\n");
    out.append("            rowtag=rowTag -- required\n");
    out.append("            encoding=utf8|utf8n|utf16|utf16le|utf16be|utf32|utf32le|utf32be -- optional, default is utf8\n");
    out.append("            maxrecordsize=<max-record-size> -- optional, default is 8192\n");
    out.append("    replicate options:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        cluster=<cluster-name>     -- replicates to (additional) cluster\n");
    out.append("        repeatlast=0|1             -- repeats last part on every node (requires cluster)\n");
    out.append("        onlyrepeated=0|1           -- ignores parts not repeated (e.g. by repeatlast)\n");
    out.append("    despray options:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        dstip=<destination-machine-ip>\n");
    out.append("        dstfile=<destination-file-path>\n");
    out.append("        dstxml=<xml-file> -- replaces dstip and dstfile\n");
    out.append("        splitprefix=... use prefix (same format as /prefix) to split file up\n");
    out.append("        wrap=0|1 -- desprays as multiple files\n");
    out.append("        multicopy=0|1   -- each destination part gets whole file\n");
    out.append("    copy options:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        dstname=<destination-logical-name>\n");
    out.append("        dstcluster=<cluster-name>\n");
    out.append("        dstclusterroxie=No|Yes <destination cluster is a roxie cluster> -- optional\n");
    out.append("        srcdali=<foreign-dali-ip> -- optional\n");
    out.append("        srcusername=<username-for-accessing-srcdali> -- optional\n");
    out.append("        srcpassword=<password-for-accessing-srcdali> -- optional\n");
    out.append("        wrap=0|1 -- copies from a larger to smaller cluster without spliting parts\n");
    out.append("        diffkeysrc=<old-key-name>   -- use keydiff/keypatch (src old name)\n");
    out.append("        diffkeydst=<old-key-name>   -- use keydiff/keypatch (dst old name)\n");
    out.append("        multicopy=0|1   -- each destination part gets whole file\n");
    out.append("    remove options:\n");
    out.append("        name=<logical-name>\n");
    out.append("        names=<multiple-logical-names-separated-by-comma>\n");
    out.append("        namelist=<logical-name-list-in-file>\n");
    out.append("        nodelete=0|1    -- optional\n");
    out.append("    rename options:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        dstname=<destination-logical-name>\n");
    out.append("    list options:\n");
    out.append("        name=<logical-name-mask>\n");
    out.append("        saveto=<path and file name to save the result>\n");
    out.append("            (more to be defined)\n");
    out.append("    addsuper options:\n");
    out.append("        superfile=<logical-name>\n");
    out.append("        subfiles=<logical-name>{,<logical-name>}  -- no spaces between logical-names.\n");
    out.append("        before=<logical-name> -- optional\n");
    out.append("    removesuper options:\n");
    out.append("        superfile=<logical-name>\n");
    out.append("        subfiles=<logical-name>{,<logical-name> -- optional. Can be *, which means all subfiles\n");
    out.append("        delete=1|0 -- optional. Whether or not actually remove the files.\n");
    out.append("    copysuper options:\n");
    out.append("        srcname=<source-super-name>\n");
    out.append("        dstname=<destination-super-name>\n");
    out.append("        dstcluster=<cluster-name>\n");
    out.append("        srcdali=<foreign-dali-ip>\n");
    out.append("        srcusername=<username-for-accessing-srcdali> -- optional\n");
    out.append("        srcpassword=<password-for-accessing-srcdali> -- optional\n");
    out.append("    listsuper options:\n");
    out.append("        superfile=<logical-name>\n");
    out.append("    savexml options:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        dstxml=<xml-file>\n");
    out.append("    add options:\n");
    out.append("        srcxml=<xml-file>\n");
    out.append("        dstname=<destination-logical-name>\n");
    out.append("        -- To add remote files from another dali directly, use these options instead of srcxml:\n");
    out.append("        srcname=<source-logical-name>\n");
    out.append("        srcdali=<source-dali-ip>\n");
    out.append("        srcusername=<user-name-for-source-dali>\n");
    out.append("        srcpassword=<password-for-source-dali>\n");
    out.append("    status options:\n");
    out.append("        wuid=<dfu-workunit-id>\n");
    out.append("    abort options:\n");
    out.append("        wuid=<dfu-workunit-id>\n");
    out.append("    resubmit options:\n");
    out.append("        wuid=<dfu-workunit-id>\n");
    out.append("    monitor options:  \n");
    out.append("        event=<eventn-name> \n");
    out.append("        lfn=<logical-name> -- either specify lfn or ip/file\n");
    out.append("        ip=<ip-for-file>\n");
    out.append("        file=<filename>\n");
    out.append("        sub=0|1\n");
    out.append("        shotlimit=<number>\n");
    out.append("    dafilesrv options:  \n");
    out.append("        idletimeout=<idle-timeout-secs> -- how long idle before stops \n");

    printf("%s",out.str());
}

bool build_globals(int argc, const char *argv[], IProperties * globals)
{
    int i;

    for(i = 0; i < argc; i++)
    {
        if(argv[i] != NULL && argv[i][0] == '@' && argv[i][1] != '\0')
        {
            globals->loadFile(argv[i]+1);
        }
    }

    for (i = 1; i < argc; i++)
    {
        if (strchr(argv[i],'='))
        {
            globals->loadProp(argv[i]);
        }
    }

    StringBuffer tmp;
    if(globals->hasProp("encrypt")) {
        encrypt(tmp.clear(),globals->queryProp("encrypt") );  // basic encryption at this stage
        globals->setProp("encrypt",tmp.str());
    }
    if(globals->hasProp("decrypt")) {
        encrypt(tmp.clear(),globals->queryProp("decrypt") );  // basic encryption at this stage
        globals->setProp("decrypt",tmp.str());
    }

    return true;
}

void promptFor(const char *prompt, const char *prop, bool hide, IProperties * globals)
{
    StringBuffer result;
    fprintf(stdout, "%s", prompt);
    fflush(stdout);
    if (hide)
    {
#ifdef _WIN32
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);   
        DWORD dwInputMode;
        GetConsoleMode(hStdIn, &dwInputMode);   
        SetConsoleMode(hStdIn, dwInputMode & ~ENABLE_LINE_INPUT & ~ENABLE_ECHO_INPUT);
        loop
        {
            /* read a character from the console input */   
            char ch;
            DWORD dwRead;
            if (!ReadFile(hStdIn, &ch, sizeof(ch), &dwRead, NULL))
                break;
            if (ch == '\n' || ch=='\r' || !ch)
                break;
            result.append(ch);
        }
        SetConsoleMode(hStdIn, dwInputMode); 
#else
        int fn = fileno(stdin);
#ifdef __linux__        
        struct termio t;
        /* If ioctl fails, we're probably not connected to a terminal. */
        if(!ioctl(fn, TCGETA, &t))
        {
            t.c_lflag &= ~ECHO;
            ioctl(fn, TCSETA, &t);
        }
#endif
        loop
        {
            char ch = fgetc(stdin);
            if (ch == '\n' || ch=='\r' || !ch)
                break;
            result.append(ch);
        }
#ifdef __linux__        
        if(!ioctl(fn, TCGETA, &t))
        {
            t.c_lflag |= ECHO;
            ioctl(fn, TCSETA, &t);
        }
#endif
#endif
        printf("\n");
    }
    else
    {
        char buf[100];
        if (fgets(buf, 100, stdin))
            result.append(buf);
        if (result.length() && result.charAt(result.length()-1)=='\n')
            result.remove(result.length()-1, 1);
    }
    globals->setProp(prop, result);
}


int main(int argc, const char* argv[])
{
    InitModuleObjects();

    if ((argc >= 2) && ((stricmp(argv[1], "/version") == 0) || (stricmp(argv[1], "-version") == 0))) 
    {
        printVersion();
        return 0;
    }

    Owned<IFile> inifile = createIFile("dfuplus.ini");
    if(argc < 2 && !(inifile->exists() && inifile->size() > 0))
    {
        handleSyntax();
        return 0;
    }

    if ((argc >= 2) && ((argv[1][0]=='/' || argv[1][0]=='-') && (argv[1][1]=='?' || argv[1][1]=='h'))) 
    {
        handleSyntax();
        return 0;
    }

    //queryLogMsgManager()->changeMonitorFilterOwn(queryStderrLogMsgHandler(), getPassNoneLogMsgFilter());

    Owned<IProperties> globals = createProperties("dfuplus.ini", true);

    if(!build_globals(argc, argv, globals))
    {
        fprintf(stderr, "ERROR: Invalid command syntax.\n");
        releaseAtoms();
        return DFUERR_InvalidCommandSyntax;
    }



    
    const char* action = globals->queryProp("action");
    if(!action || !*action)
    {
        handleSyntax();
        fprintf(stderr, "\nERROR: please specify one action");
        releaseAtoms();
        return DFUERR_TooFewArguments;
    }

    const char* server = globals->queryProp("server");
    if (!server || !*server) {
        if (stricmp(action,"dafilesrv")==0)
            globals->setProp("server","127.0.0.1"); // dummy
        else { 
            fprintf(stderr, "ERROR: Esp server url not specified.\n");
            releaseAtoms();
            return DFUERR_TooFewArguments;
        }
    }

    try
    {
        Owned<CDfuPlusHelper> helper = new CDfuPlusHelper(LINK(globals.get()));
        helper->doit();
    }
    catch(IException* e)
    {
        StringBuffer errmsg;
        e->errorMessage(errmsg);
        fprintf(stderr, "%s\n", errmsg.str());
    }
    
    releaseAtoms();
    return 0;
}
