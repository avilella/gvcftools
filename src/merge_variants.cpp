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

#include "gvcftools.hh"
#include "related_sample_util.hh"
#include "ref_util.hh"
#include "tabix_util.hh"
#include "trio_option_util.hh"

#include "boost/program_options.hpp"

#include <ctime>

#include <iostream>
#include <string>
#include <vector>


std::ostream& log_os(std::cerr);
std::ostream& report_os(std::cout);

std::string cmdline;

enum sample_t {
    MOTHER,
    FATHER,
    CHILD,
    SAMPLE_SIZE
};

const char* sample_label[] = { "mother" , "father" , "child" };


enum parent_state_t {
    SAMEHOM,
    DIFFHOM,
    SAMEHET,
    DIFFHET,
    HOMHET,
    PARENT_SIZE
};

const char* parent_state_label[] = { "samehom" , "diffhom" , "samehet" , "diffhet" , "homhet" };


enum child_state_t {
    HOM,
    HET,
    CHILD_SIZE
};

const char* child_state_label[] = { "hom" , "het" };



static
parent_state_t
get_pt_cat(const bool ismhom,
           const bool isfhom,
           const char m1,
           const char f1,
           const char m2,
           const char f2){

    if(ismhom!=isfhom) return HOMHET;

    if(ismhom) {
        return (m1==f1 ? SAMEHOM : DIFFHOM);
    } else {
        return ((m1==f1) && (m2==f2) ? SAMEHET : DIFFHET);
    }
}



struct site_stats : public site_stats_core<SAMPLE_SIZE> {

    site_stats()
    {
        for(unsigned i(0);i<PARENT_SIZE;++i) {
            for(unsigned j(0);j<CHILD_SIZE;++j) {
                snp_correct_type[i][j] = 0;
                incorrect_type[i][j] = 0;
            }
        }
    }

    unsigned snp_correct_type[PARENT_SIZE][CHILD_SIZE];
    unsigned incorrect_type[PARENT_SIZE][CHILD_SIZE];
};



static
void
processSite(const site_crawler* sa,
            const pos_t low_pos,
            const reference_contig_segment& ref_seg,
            pos_reporter& pr,
            pos_reporter& pr_ah,
            pos_reporter& pr_hethethom,
            site_stats& ss) {

    bool is_all_mapped(true);
    bool is_any_mapped(false);

    bool is_all_called(true);
    bool is_any_called(false);
    
    const char ref_base(ref_seg.get_base(low_pos-1));

    if(ref_base=='N') return;

    for(unsigned st(0);st<SAMPLE_SIZE;++st) {
        if(sa[st].is_pos_valid() && (sa[st].pos==low_pos) && (sa[st].n_total != 0)) {
            ss.sample_mapped[st]++;
            is_any_mapped=true;
        } else {
            is_all_mapped=false;
        }

        if(sa[st].is_pos_valid() && (sa[st].pos==low_pos) && sa[st].is_call){
            ss.sample_called[st]++;
            if(! ((ref_base==sa[st].allele[0]) && (ref_base==sa[st].allele[1]))){
                ss.sample_snp[st]++;
                if(sa[st].allele[0] != sa[st].allele[1]){
                    ss.sample_snp_het[st]++;
                }
            }
            is_any_called=true;
        } else {
            is_all_called=false;
        }
    }

    if(! is_all_mapped) {
        if(is_any_mapped) ss.some_mapped++;
    } else {
        ss.all_mapped++;
    }

    if(! is_all_called) {
        if(is_any_called) ss.some_called++;
        return;
    } else {
        ss.all_called++;
    }

    const char c1(sa[CHILD].allele[0]);
    const char c2(sa[CHILD].allele[1]);
    const char f1(sa[FATHER].allele[0]);
    const char f2(sa[FATHER].allele[1]);
    const char m1(sa[MOTHER].allele[0]);
    const char m2(sa[MOTHER].allele[1]);

    const bool isc1f((c1==f1) || (c1==f2));
    const bool isc1m((c1==m1) || (c1==m2));
    const bool isc2f((c2==f1) || (c2==f2));
    const bool isc2m((c2==m1) || (c2==m2));

    const bool is_correct((isc1f && isc2m) || (isc1m && isc2f));
    
    const bool ischom(c1==c2);
    const bool ismhom(m1==m2);
    const bool isfhom(f1==f2);

    if(is_correct) {
        const bool is_ref_call(ischom && ismhom && isfhom && (c1==ref_base));
        if(! is_ref_call){

            // find wendy's categories:
            const parent_state_t pt(get_pt_cat(ismhom,isfhom,m1,f1,m2,f2));
            const child_state_t ct( ischom ? HOM : HET );

            ss.snp_correct++;
            ss.snp_correct_type[pt][ct]++;

            if     (! ismhom) ss.sample_snp_correct_het[MOTHER]++;
            else if(m1!=ref_base) ss.sample_snp_correct_hom[MOTHER]++;
            if     (! isfhom) ss.sample_snp_correct_het[FATHER]++;
            else if(f1!=ref_base) ss.sample_snp_correct_hom[FATHER]++;
            if     (! ischom) ss.sample_snp_correct_het[CHILD]++;
            else if(c1!=ref_base) ss.sample_snp_correct_hom[CHILD]++;

            if((! ismhom) && (! isfhom) && (! ischom) && (m1==f1) && (m1==c1) && (m2==f2) && (m2==c2)){
                pr_ah.print_pos(sa);
            }


            if((! ismhom) && (! isfhom) && (ischom) && (c1!=ref_base)  ){ 
              pr_hethethom.print_pos(sa);
            }
        }
    } else {
        pr.print_pos(sa);

        // find wendy's categories:
        const parent_state_t pt(get_pt_cat(ismhom,isfhom,m1,f1,m2,f2));
        const child_state_t ct( ischom ? HOM : HET );
        
        ss.incorrect++;
        ss.incorrect_type[pt][ct]++;
    }
}



static
void
report(const site_stats& ss,
       const time_t& start_time,
       const bool is_variable_metadata){

    std::ostream& os(report_os);

    os << "CMDLINE " << cmdline << "\n";
    if(is_variable_metadata) {
        os << "START_TIME " << asctime(localtime(&start_time));
        os << "VERSION " << gvcftools_version() << "\n";
    }
    os << "\n";
    os << "sites: " << ss.ref_size << "\n";
    os << "known_sites: " << ss.known_size << "\n";
    os << "\n";
    for(unsigned st(0);st<SAMPLE_SIZE;++st) {
        os << "sites_mapped_" << sample_label[st] << ": " << ss.sample_mapped[st] << "\n";
    }

    if(ss.known_size <(ss.some_mapped+ss.all_mapped)) {
        log_os << "ERROR: known_size is less than mapped sites. ks: " << ss.known_size << " " << " sm: " << ss.some_mapped << " am: " << ss.all_mapped << "\n";
        exit(EXIT_FAILURE);
    }

    assert(ss.known_size >= (ss.some_mapped+ss.all_mapped));
    const unsigned none_mapped(ss.known_size-(ss.some_mapped+ss.all_mapped));
    os << "sites_mapped_in_no_samples: " << none_mapped << "\n";
    os << "sites_mapped_in_some_samples: " << ss.some_mapped << "\n";
    os << "sites_mapped_in_all_samples: " << ss.all_mapped << "\n";
    os << "\n";
    for(unsigned st(0);st<SAMPLE_SIZE;++st) {
        os << "sites_called_" << sample_label[st] << ": " << ss.sample_called[st] << "\n";
    }
    assert(ss.known_size >= (ss.some_called+ss.all_called));
    const unsigned none_called(ss.known_size-(ss.some_called+ss.all_called));
    os << "sites_called_in_no_samples: " << none_called << "\n";
    os << "sites_called_in_some_samples: " << ss.some_called << "\n";
    os << "sites_called_in_all_samples: " << ss.all_called << "\n";
    os << "sites_called_in_all_samples_conflict: " << ss.incorrect << "\n";
    os << "fraction_of_known_sites_called_in_all_samples: " << ratio(ss.all_called,ss.known_size) << "\n";
    os << "fraction_of_sites_called_in_all_samples_in_conflict: " << ratio(ss.incorrect,ss.all_called) << "\n";
    os << "\n";
    const unsigned snps(ss.snp_correct+ss.incorrect);
    for(unsigned st(0);st<SAMPLE_SIZE;++st) {
        const unsigned het(ss.sample_snp_het[st]);
        const unsigned hom(ss.sample_snp[st]-ss.sample_snp_het[st]);
        const double sample_het_hom(ratio(het,hom));
        const double sample_phet(ratio(het,(hom+het)));  
        os << "sites_with_snps_called_total_het_hom_het/hom_P(het)_" << sample_label[st] << ": " << ss.sample_snp[st] << " " << het << " " << hom << " " << sample_het_hom << " " << sample_phet << "\n";
    }
    os << "sites_called_in_all_samples_with_snps_called_any_sample: " << snps << "\n";
    os << "fraction_of_snp_sites_in_conflict: " << ratio(ss.incorrect,snps) << "\n";
    os << "\n";

    for(unsigned st(0);st<SAMPLE_SIZE;++st) {
        const unsigned het(ss.sample_snp_correct_het[st]);
        const unsigned hom(ss.sample_snp_correct_hom[st]);
        const double sample_het_hom(ratio(het,hom));  
        const double sample_phet(ratio(het,(hom+het)));  
        os << "snp_non_conflict_total_het_hom_het/hom_P(het)_" << sample_label[st] << ": " << (het+hom) << " " << het << " " << hom << " " << sample_het_hom << " " << sample_phet << "\n";
    }
    os << "\n";

    for(unsigned i(0);i<PARENT_SIZE;++i) {
        for(unsigned j(0);j<CHILD_SIZE;++j) {
            os << "snp_conflict_type_parent-" << parent_state_label[i] 
               << "_child-" << child_state_label[j] << ": " 
               << ss.incorrect_type[i][j] << "\n";
        }
    }
    os << "\n";
    for(unsigned i(0);i<PARENT_SIZE;++i) {
        for(unsigned j(0);j<CHILD_SIZE;++j) {
            os << "snp_non_conflict_type_parent-" << parent_state_label[i] 
               << "_child-" << child_state_label[j] << ": " 
               << ss.snp_correct_type[i][j] << "\n";
        }
    }
    os << "\n";
    const unsigned shehe(ss.snp_correct_type[SAMEHET][HET]);
    const unsigned sheho(ss.snp_correct_type[SAMEHET][HOM]);
    const unsigned sheall(shehe+sheho);
    const double shehetp(ratio(shehe,sheall));
    os << "P(child-het|parent-samehet) for non-conflicting snps (neutral site expect 1/2): " << shehetp << "\n";
    const unsigned dhoall(ss.snp_correct_type[DIFFHOM][HET]+ss.snp_correct_type[DIFFHOM][HOM]);
    const double shealltp(ratio(sheall,(sheall+dhoall)));
    os << "P(parent-samehet|(parent-samehet or parent-diffhom)) for non-conflicting snps (neutral site expect 2/3): " << shealltp << "\n";
}


#if 0
static
void
disallow_option(const boost::program_options::variables_map& vm,
                const char* label) {

    if(! vm.count(label)) return;

    log_os << "ERROR:: option '" << label << "' is not allowed in selected snp-mode\n";
    exit(EXIT_FAILURE);
}
#endif



// convenience struct for all pos reporters:
struct pos_reporters {

    pos_reporters(const std::string& conflict_pos_file,
                  const std::string& allhet_pos_file,
                  const std::string& hethethom_pos_file) 
        : slabel(sample_label,sample_label+SAMPLE_SIZE)
        , conflict(conflict_pos_file,slabel)
        , allhet(allhet_pos_file,slabel)
        , hethethom(hethethom_pos_file,slabel)
    {}

    std::vector<std::string> slabel;
    pos_reporter conflict;
    pos_reporter allhet;
    pos_reporter hethethom;
};



static
void
accumulate_region_statistics(const sample_info* const si,
                             const shared_crawler_options& opt,
                             const std::string& ref_seq_file,
                             const char* region,
                             pos_reporters& pr,
                             site_stats& ss) {

    // setup reference sequence:
    reference_contig_segment ref_seg;
    unsigned segment_known_size;
    get_samtools_std_ref_segment(ref_seq_file.c_str(),region,ref_seg,segment_known_size);
    if(opt.is_region()){
        ref_seg.set_offset(opt.region_begin-1);
    }

    ss.ref_size += ref_seg.seq().size();
    ss.known_size += segment_known_size;

    // setup allele crawlers:
    site_crawler sa[SAMPLE_SIZE] = { site_crawler(si[MOTHER],MOTHER,opt,region,ref_seg),
                                     site_crawler(si[FATHER],FATHER,opt,region,ref_seg),
                                     site_crawler(si[CHILD],CHILD,opt,region,ref_seg) };

    while(true) {
        // get lowest position:
        bool is_low_pos_set(false);
        pos_t low_pos(0);
        for(unsigned st(0);st<SAMPLE_SIZE;++st) {
            if(! sa[st].is_pos_valid()) continue;
            if((! is_low_pos_set) || (sa[st].pos < low_pos)){
                low_pos = sa[st].pos;
                is_low_pos_set=true;
            }
        }
        if(! is_low_pos_set) break;
        
        processSite(sa,low_pos,ref_seg,pr.conflict,pr.allhet,pr.hethethom,ss);

        for(unsigned st(0);st<SAMPLE_SIZE;++st) {
            if(sa[st].is_pos_valid() && (low_pos == sa[st].pos)){
                sa[st].update();
            }
        }
    }
}



static
void
try_main(int argc,char* argv[]){

    const time_t start_time(time(0));
    const char* progname(compat_basename(argv[0]));
    
    for(int i(0);i<argc;++i){
        if(i) cmdline += ' ';
        cmdline += argv[i];
    }

    std::vector<std::string> input_files;
    std::string ref_seq_file;
    shared_crawler_options opt;

    namespace po = boost::program_options;
    po::options_description req("configuration");
    req.add_options()
        ("ref", po::value<std::string>(&ref_seq_file),"samtools reference sequence (required)")
        ("region", po::value<std::string>(&opt.region), "samtools reference region (optional)")
        ("input", po::value<std::vector<std::string> >(&input_files)->multitoken(), "merge files (can be specified multiple times)");

    po::options_description help("help");
    help.add_options()
        ("help,h","print this message");

    po::options_description visible("options");
    visible.add(req).add(help);


    bool po_parse_fail(false);
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, visible,
                  po::command_line_style::unix_style ^ po::command_line_style::allow_short), vm);
        po::notify(vm);    
    } catch(const boost::program_options::error& e) { // todo:: find out what is the more specific exception class thrown by program options
        log_os << "\nERROR: Exception thrown by option parser: " << e.what() << "\n";
        po_parse_fail=true;
    }
    
    if ((argc<=1) || (vm.count("help")) || po_parse_fail) {
        log_os << "\n" << progname << " merge the variants from multiple gVCF files\n\n";
        log_os << "version: " << gvcftools_version() << "\n\n";
        log_os << "usage: " << progname << " [options] > merged_variants\n\n";
        log_os << visible << "\n";
        exit(EXIT_FAILURE);
    }


    if(opt.is_region()) {
        parse_tabix_region(si[MOTHER].file.c_str(),opt.region.c_str(),opt.region_begin,opt.region_end);
        opt.region_begin+=1;
    }

    pos_reporters pr(conflict_pos_file,allhet_pos_file,hethethom_pos_file);
    site_stats ss;

    if(opt.is_region()) {
        accumulate_region_statistics(si,opt,ref_seq_file,opt.region.c_str(),pr,ss);
    } else {
        fasta_chrom_list fcl(ref_seq_file.c_str());
        while(true) {
            const char* chrom = fcl.next();
            if(NULL == chrom) break;
            // don't even bother making this efficient:
            bool is_skip(false);
            for (unsigned i(0);i<exclude_list.size();++i) {
                if(strcmp(chrom,exclude_list[i].c_str())==0) {
                    is_skip=true;
                    break;
                }
            }
            if(is_skip) {
                log_os << "skipping chromosome: '" << chrom << "'\n";
            } else {
                log_os << "processing chromosome: '" << chrom << "'\n";
                accumulate_region_statistics(si,opt,ref_seq_file,chrom,pr,ss);
            }
        }

    }
    report(ss,start_time,is_variable_metadata);
}



static
void
dump_cl(int argc,
        char* argv[],
        std::ostream& os) {
 
    os << "cmdline:";
    for(int i(0);i<argc;++i){
        os << ' ' << argv[i];
    }
    os << std::endl;
}



int
main(int argc,char* argv[]){

    std::ios_base::sync_with_stdio(false);

    // last chance to catch exceptions...
    //
    try{
        try_main(argc,argv);

    } catch(const std::exception& e) {
        log_os << "FATAL:: EXCEPTION: " << e.what() << "\n"
               << "...caught in main()\n";
        dump_cl(argc,argv,log_os);
        exit(EXIT_FAILURE);

    } catch(...) {
        log_os << "FATAL:: UNKNOWN EXCEPTION\n"
               << "...caught in main()\n";
        dump_cl(argc,argv,log_os);
        exit(EXIT_FAILURE);
    }
    return EXIT_SUCCESS;
}