// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Copyright (c) 2009-2012 Illumina, Inc.
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//

/// \file
///
/// \author Chris Saunders
///


#include "related_sample_util.hh"
#include "tabix_streamer.hh"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>

#include <fstream>
#include <iostream>



namespace {
std::ostream& log_os(std::cerr);
}



static
bool
get_format_key_index(const char* format,
                     const char* key,
                     unsigned& index) {
    index=0;
    do {
        if(index) format++;
        if(0==strncmp(format,key,strlen(key))) return true;
        index++;
    } while(NULL != (format=strchr(format,':')));
    return false;
}


bool
snp_type_info_vcf::
get_allele(std::pair<char,char>& allele,
           const char * const * word,
           const unsigned offset,
           const char ref_base) const {

    // get the genotype code numbers:
    unsigned gtcode[2];
    if(! get_digt_code(word,gtcode)) { return false; }

    // in this case, pull result form reference sequence:
    if((gtcode[0] == 0) && (gtcode[1] == 0)) {
        allele = std::make_pair(ref_base,ref_base);
        return true;
    }

    const char ref(word[REF_COL][offset]);
    char altbase[2] = {ref,ref};
    for(unsigned i(0);i<2;++i) {
        if(gtcode[i]==0) continue;
        const char* alt(word[ALT_COL]);
        for(unsigned ai(0);(ai+1)<gtcode[i];alt++) {
            if(! *alt) return false;
            if((*alt)==',') ai++;
        }
        altbase[i]=alt[offset];
    }
    allele = std::make_pair(altbase[0],altbase[1]);
    return true;
}



char*
snp_type_info_vcf::
get_format_string(const char* const * word,
                  const char* key) {

    unsigned keynum(0);
    if(! get_format_key_index(word[FORMAT_COL],key,keynum)) return NULL;

    const char* sample(word[SAMPLE_COL]);
    for(;keynum;sample++) {
        if(! *sample) return NULL;
        if((*sample)==':') keynum--;
    }
    unsigned len(0);
    while(sample[len]!='\0' && (sample[len]!=':')) {
        len++;
    }
    return strndup(sample,len);
}



bool
snp_type_info_vcf::
get_digt_code(const char * const * word,
              unsigned digt_code[2]) {

    // get the genotype code numbers:
    for(unsigned i(0);i<2;++i) {
        digt_code[i] = 0;
    }

    char* gtword=get_format_string(word,"GT");
    if(NULL==gtword) return false;

    int ret = sscanf(gtword,"%i/%i",&digt_code[0],&digt_code[1]);
    if(2!=ret) {
        ret = sscanf(gtword,"%i|%i",&digt_code[0],&digt_code[1]);
    }
    free(gtword);
    return (2==ret);
}



// extract unsigned value for key from the vcf info field
bool
snp_type_info_vcf::
get_info_unsigned(const char* info,
                  const char* key,
                  unsigned& val) {

    const char* tmp(NULL);
    do {
        if(NULL != tmp) info=tmp+1;
        if(0!=strncmp(info,key,strlen(key))) continue;
        if(NULL==(info=strchr(info,'='))) return false;
        const char* s(info+1);
        val=parse_unsigned(s);
        return true;
    } while(NULL != (tmp=strchr(info,';')));
    return false;
}



// extract float value for key from the vcf info field
bool
snp_type_info_vcf::
get_info_float(const char* info,
               const char* key,
               float& val) {

    const char* tmp(NULL);
    do {
        if(NULL != tmp) info=tmp+1;
        if(0!=strncmp(info,key,strlen(key))) continue;
        if(NULL==(info=strchr(info,'='))) return false;
        const char* s(info+1);
        val=parse_double(s);
        return true;
    } while(NULL != (tmp=strchr(info,';')));
    return false;
}



bool
snp_type_info_vcf::
get_format_float(const char* const * word,
                 const char* key,
                 float& val) {

    unsigned keynum(0);
    if(! get_format_key_index(word[FORMAT_COL],key,keynum)) return false;

    const char* sample(word[SAMPLE_COL]);
    for(;keynum;sample++) {
        if(! *sample) return false;
        if((*sample)==':') keynum--;
    }
    val=parse_double(sample);
    return true; 
}



site_crawler::
site_crawler(const sample_info& si,
             const unsigned sample_id,
             const shared_crawler_options& opt,
             const char* chr_region,
             const reference_contig_segment& ref_seg)
    : pos(0)
    , is_call(false)
    , _chrom(NULL)
    , _si(si)
    , _sample_id(sample_id)
    , _opt(opt)
    , _chr_region(chr_region)
    , _tabs(NULL)
    , _is_sample_begin_state(true)
    , _is_sample_end_state(false)
    , _next_file(0)
    , _ref_seg(ref_seg)
    , _n_word(0)
    , _locus_size(0)
    , _locus_offset(0)
    , _skip_call_begin_pos(0)
    , _skip_call_end_pos(0)
{
    update();
}



site_crawler::
~site_crawler() {
    if(NULL != _tabs) delete _tabs;
}





void
site_crawler::
dump_state(std::ostream& os) const {
    const std::string& afile(_si.file);
    os << "SITE_CRAWLER STATE:\n";
    os << "\tchrom: " << chrom();
    os << "\tposition: " << pos << " offset: " << _locus_offset << "\n";
    os << "\tfile: '" << afile << "'\n";
    os << "\tline: '";
    dump_line(os);
    os << "'\n";
}



static const char sep('\t');


// return true if current position in record is valid and usable
bool
site_crawler::
process_record_line(char* line) {
    static const unsigned MAX_WORD(50);
    
    // do a low-level tab parse:
    {
        char* p(line);
        _word[0]=p;
        _n_word=1;
        while(true){
            if((*p == '\n') || (*p == '\0')) break;
            if (*p == sep) {
                *p = '\0';
                _word[_n_word++] = p+1;
                if(_n_word == MAX_WORD) break;
            }
            ++p;
        }
        // allow for optional extra columns in each file format:
        if(_n_word<_opt.sti().col_count()){
            log_os << "ERROR: Consensus record has " << _n_word << " column(s) but expecting at least " << _opt.sti().col_count() << "\n";
            dump_state(log_os);
            exit(EXIT_FAILURE);
        }
    }

    const pos_t last_pos(pos);
    _chrom=_opt.sti().chrom(_word);
    pos=_opt.sti().pos(_word);

    const bool is_indel=_opt.sti().get_is_indel(_word);

    if(pos<1) {
        log_os << "ERROR: gcvf record position less than 1. position: " << pos << " "; 
        dump_state(log_os);
        exit(EXIT_FAILURE);
    }

    if(_opt.is_region()) {
        // deal with vcf records after the region of interest:
        if(pos>_opt.region_end) {
            _is_sample_begin_state = false;
            _is_sample_end_state = true;
            return true;
        }
    } else {
        if(pos>static_cast<pos_t>(_ref_seg.end())) {
            log_os << "ERROR: allele file position exceeds final position in reference sequence segment . position: " 
                   << pos << " ref_contig_end: " << _ref_seg.end() << "\n";
            dump_state(log_os);
            exit(EXIT_FAILURE);
        }
    }

    if(! _opt.sti().get_nonindel_ref_length(pos,is_indel,_word,_locus_size)) {
        //log_os << "ERROR: failed to parse locus at pos: "  << pos << "\n";
        log_os << "WARNING: failed to parse locus at pos: "  << pos << "\n";
        dump_state(log_os);
        //exit(EXIT_FAILURE);
        _locus_size=0;
    }

    _locus_offset=0;

    // deal with vcf records which fully preceed the region of interest:
    if(_opt.is_region()) {
        if((pos+_locus_size-1)<_opt.region_begin) return false;
    }

    const bool last_is_call(is_call);
    is_call = _opt.sti().get_is_call(_word,pos,is_indel,_skip_call_begin_pos,_skip_call_end_pos);

    n_total = _opt.sti().total(_word);

    if(is_indel) {
       pos=last_pos;
       _locus_size=0;
       return false;
    }

    if(is_call) {
        const char ref_base=_ref_seg.get_base(pos-1);
        if(! _opt.sti().get_allele(allele,_word,_locus_offset,ref_base)) {
            is_call=false;
            //log_os << "ERROR: Failed to read site genotype from record:\n";
            //dump_state(log_os);
            //exit(EXIT_FAILURE);
        } else {
            strcpy(score,_opt.sti().score(_word));
        }
    }

    // don't allow failed block read-through, so that we can get through indel-overlap errors
    //if(! is_call) {
    //    if(_locus_size>1) _locus_size=1;
    //}
    
    if(! _is_sample_begin_state) {
        // we've already filtered out indels, so we can require that valid records do not have the same pos or lower.
        if(pos<=last_pos) {
            if(_opt.is_murdock_mode) {
                pos=last_pos; 
                _locus_size=0;
                return false;
                //pos=last_pos;
                //is_call=last_is_call;
            } else {
                log_os << "ERROR: unexpected position order in variant file. current_pos: " 
                       << pos << " last_pos: " << last_pos << "\n";
                dump_state(log_os);
                exit(EXIT_FAILURE);
            }
        }
    } else {
        _is_sample_begin_state=false;
    }       

    // deal with vcf records which partially overlap the region of interest:
    if(_opt.is_region()) {
        if(pos<_opt.region_begin) return false;
    }

    return true;
}



void
site_crawler::
update(){
    if(_is_sample_end_state) return;


    // move on to a new locus:
    while(true) {
        // continue crawling through a multibase locus:
        //
        // at present (201202) this only means compressed block
        // entries in gVCF -- long indels should be skipped over
        // rather than walked
        //
        _locus_offset++;
        if(_locus_offset < _locus_size) {
            // check for pos moving past the end of region of interest:
            if(_opt.is_region()) {
                if((pos+1)>(_opt.region_end)){
                    _is_sample_begin_state = false;
                    _is_sample_end_state = true;
                    return;
                }
            }

            pos++;

            if(_opt.is_region()) {
                // check for pos preceeding the start of region of interest in a multi-base record:
                if((pos)<(_opt.region_begin)){
                    continue;
                }
            }

            if(pos>static_cast<pos_t>(_ref_seg.end())) {
                log_os << "ERROR: allele file position exceeds final position in reference sequence segment. position: " 
                       << pos << " ref_contig_end: " << _ref_seg.end() << "\n";
                dump_state(log_os);
                exit(EXIT_FAILURE);
            }

            if(is_call) {
                const char ref_base=_ref_seg.get_base(pos-1);
                if(! _opt.sti().get_allele(allele,_word,_locus_offset,ref_base)) {
                    log_os << "ERROR: Failed to read site genotype from record:\n";
                    dump_state(log_os);
                    exit(EXIT_FAILURE);
                }
            }
            return;
        }

        // start new/next file:
        if(NULL == _tabs) {
            if(_next_file >= 1){
                _is_sample_begin_state = false;
                _is_sample_end_state = true;
                return;
            }
            const std::string& afile(_si.file);
            _tabs=new tabix_streamer(afile.c_str(),_chr_region);
            if(! _tabs) {
                log_os << "ERROR:: Can't open gvcf file: " << afile << "\n";
                exit(EXIT_FAILURE);
            }
            _next_file++;
        }

        // read through file to get to a data line:
        bool is_eof(true);
        char* line(NULL);
        while(_tabs->next()) {
            line=_tabs->getline();
            if(NULL == line) break;
            assert(strlen(line));
            if(line[0] == '#') continue;
            is_eof=false;
            break;
        }

        if(! is_eof) {
            const bool is_valid=process_record_line(line);
            if(is_valid) return;
        } else {
            // if eof, go to next file or terminate:
            delete _tabs;
            _tabs=NULL;
        }
    }
}



void
site_crawler::
dump_line(std::ostream& os) const {
    if(_is_sample_end_state) return;
    for(unsigned i(0);i<_n_word;++i){
        if(i) os << sep;
        os << _word[i];
    }
}



pos_reporter::
pos_reporter(const std::string& filename,
             const std::vector<std::string>& sample_label)
    : is_pos_report(false),
      pos_fs_ptr(0),
      sample_size(sample_label.size()),
      _sample_label(sample_label) {
    
    if(filename.empty()) return;
    pos_fs_ptr = new std::ofstream(filename.c_str());
    if(! pos_fs_ptr) {
        log_os << "ERROR: Failed to allocate stream pointer for file: " << filename << "\n";
        exit(EXIT_FAILURE);
    }
    if(! *pos_fs_ptr) {
        log_os << "ERROR: Failed to open output file: " << filename << "\n";
        exit(EXIT_FAILURE);
    }
    is_pos_report=true;
}



pos_reporter::    
~pos_reporter() {
    if(is_pos_report) delete pos_fs_ptr;
}



void
pos_reporter::
print_pos(const site_crawler* sa) {
    if(! is_pos_report) return;
    *pos_fs_ptr << "EVENT\t" << sa[0].chrom() << "\t" << sa[0].pos << "\n";
    for(unsigned i(0);i<sample_size;++i){
        *pos_fs_ptr << _sample_label[i] << "\t";
        *pos_fs_ptr << "\t";
        sa[i].dump_line(*pos_fs_ptr);
        *pos_fs_ptr << "\n";
    }
}
