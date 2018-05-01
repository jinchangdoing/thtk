/*
 * Redistribution and use in source and binary forms, with
 * or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain this list
 *    of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce this
 *    list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */
#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "program.h"
#include "thecl.h"
#include "util.h"
#include "mygetopt.h"

extern const thecl_module_t th06_ecl;
extern const thecl_module_t th10_ecl;

eclmap_t* g_eclmap_opcode = NULL;
eclmap_t* g_eclmap_global = NULL;
bool g_ecl_rawoutput = false;

thecl_t*
thecl_new(
    void)
{
    thecl_t* ecl = calloc(1, sizeof(thecl_t));
    list_init(&ecl->subs);
    list_init(&ecl->local_data);
    return ecl;
}

static void
thecl_free(
    thecl_t* ecl)
{
    for (size_t a = 0; a < ecl->anim_count; ++a)
        free(ecl->anim_names[a]);
    free(ecl->anim_names);

    for (size_t e = 0; e < ecl->ecli_count; ++e)
        free(ecl->ecli_names[e]);
    free(ecl->ecli_names);

    thecl_sub_t* sub;
    list_for_each(&ecl->subs, sub) {
        free(sub->name);

        thecl_instr_t* instr;
        list_for_each(&sub->instrs, instr)
            thecl_instr_free(instr);
        list_free_nodes(&sub->instrs);

        for (size_t v = 0; v < sub->var_count; ++v)
            free(sub->vars[v]);
        free(sub->vars);

        thecl_label_t* label;
        list_for_each(&sub->labels, label)
            free(label);
        list_free_nodes(&sub->labels);

        free(sub);
    }
    list_free_nodes(&ecl->subs);

    thecl_local_data_t* local_data;
    list_for_each(&ecl->local_data, local_data)
        free(local_data);
    list_free_nodes(&ecl->local_data);

    free(ecl);
}

thecl_instr_t*
thecl_instr_new(void)
{
    thecl_instr_t* instr = calloc(1, sizeof(thecl_instr_t));
    instr->type = THECL_INSTR_INSTR;
    list_init(&instr->params);
    return instr;
}

thecl_instr_t*
thecl_instr_time(unsigned int time)
{
    thecl_instr_t* instr = thecl_instr_new();
    instr->type = THECL_INSTR_TIME;
    instr->time = time;
    return instr;
}

thecl_instr_t*
thecl_instr_rank(unsigned int rank)
{
    thecl_instr_t* instr = thecl_instr_new();
    instr->type = THECL_INSTR_RANK;
    instr->rank = rank;
    return instr;
}

thecl_instr_t*
thecl_instr_label(unsigned int offset)
{
    thecl_instr_t* instr = thecl_instr_new();
    instr->type = THECL_INSTR_LABEL;
    instr->offset = offset;
    return instr;
}

void
thecl_instr_free(thecl_instr_t* instr)
{
    free(instr->string);

    thecl_param_t* param;
    list_for_each(&instr->params, param) {
        value_free(&param->value);
        free(param);
    }
    list_free_nodes(&instr->params);

    free(instr);
}

thecl_param_t*
param_new(
    int type)
{
    thecl_param_t* param = calloc(1, sizeof(thecl_param_t));
    param->type = type;
    param->value.type = type;
    param->is_expression_param = 0;
    return param;
}

void
param_free(
    thecl_param_t* param)
{
    value_free(&param->value);
    free(param);
}

static void
free_eclmaps(void)
{
    eclmap_free(g_eclmap_opcode);
    eclmap_free(g_eclmap_global);
}

static void
print_usage(void)
{
    printf("Usage: %s [-Vr] [[-c | -d] VERSION] [-m ECLMAP]... [INPUT [OUTPUT]]\n"
           "Options:\n"
           "  -c  create ECL file\n"
           "  -d  dump ECL file\n"
           "  -V  display version information and exit\n"
           "  -m  use map file for translating mnemonics\n"
           "  -r  output raw ECL opcodes, applying minimal transformations\n"
           "VERSION can be:\n"
           "  6, 7, 8, 9, 95, 10, 103 (for Uwabami Breakers), 11, 12, 125, 128, 13, 14, 143, 15, or 16\n"
           "Report bugs to <" PACKAGE_BUGREPORT ">.\n", argv0);
}

int
main(int argc, char* argv[])
{
    FILE* in = stdin;
    FILE* out = stdout;
    unsigned int version = 0;
    int mode = -1;
    const thecl_module_t* module = NULL;

    current_input = "(stdin)";
    current_output = "(stdout)";

    g_eclmap_opcode = eclmap_new();
    g_eclmap_global = eclmap_new();
    atexit(free_eclmaps);

    argv0 = util_shortname(argv[0]);
    int opt;
    int ind=0;
    while(argv[util_optind]) {
        switch(opt = util_getopt(argc, argv, ":c:d:Vm:r")) {
        case 'c':
        case 'd':
            if(mode != -1) {
                fprintf(stderr,"%s: More than one mode specified\n", argv0);
                print_usage();
                exit(1);
            }
            mode = opt;
            version = parse_version(util_optarg);
            break;
        case 'm': {
            FILE* map_file = NULL;
            map_file = fopen(util_optarg, "r");
            if (!map_file) {
                fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
                    argv0, util_optarg, strerror(errno));
                exit(1);
            }
            eclmap_load(g_eclmap_opcode, g_eclmap_global, map_file, util_optarg);
            fclose(map_file);
            break;
        }
        case 'r':
            g_ecl_rawoutput = true;
            break;
        default:
            util_getopt_default(&ind,argv,opt,print_usage);
        }
    }
    argc = ind;
    argv[argc] = NULL;

    switch (version) {
    case 6:
    case 7:
    case 8:
    case 9:
    case 95:
        module = &th06_ecl;
        break;
    case 10:
    case 103:
    case 11:
    case 12:
    case 125:
    case 128:
    case 13:
    case 14:
    case 143:
    case 15:
    case 16:
        module = &th10_ecl;
        break;
    default:
        if (mode == 'c' || mode == 'd') {
            if (version == 0)
                fprintf(stderr, "%s: version must be specified\n", argv0);
            else
                fprintf(stderr, "%s: version %u is unsupported\n", argv0, version);
            exit(1);
        }
    }

    switch (mode)
    {
    case 'c':
    case 'd': {
        if(g_ecl_rawoutput) {
            if (mode != 'd') {
                fprintf(stderr, "%s: 'r' option cannot be used while compiling\n", argv0);
                exit(1);
            }
        }

        if (0 < argc) {
            current_input = argv[0];
            in = fopen(argv[0], "rb");
            if (!in) {
                fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
                    argv0, argv[0], strerror(errno));
                exit(1);
            }
            if (1 < argc) {
                current_output = argv[1];
                out = fopen(argv[1], "wb");
                if (!out) {
                    fprintf(stderr, "%s: couldn't open %s for writing: %s\n",
                        argv0, argv[1], strerror(errno));
                    fclose(in);
                    exit(1);
                }
            }
        }

        if (mode == 'c') {
#ifdef WIN32
            _setmode(fileno(stdout), _O_BINARY);
#endif
            thecl_t* ecl = module->parse(in, version);
            if (!ecl)
                exit(1);
            module->compile(ecl, out);
            thecl_free(ecl);
        } else if (mode == 'd') {
#ifdef WIN32
            _setmode(fileno(stdin), _O_BINARY);
#endif
            thecl_t* ecl = module->open(in, version);
            if (!ecl)
                exit(1);
            module->trans(ecl);
            module->dump(ecl, out);
            thecl_free(ecl);
        }
        fclose(in);
        fclose(out);
        
        exit(0);
    }
    default:
        print_usage();
        exit(1);
    }
}
