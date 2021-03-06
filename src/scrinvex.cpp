//
//  scrinvex.cpp
//  scrinvex
//
//  Created by Aaron Graubert on 2/26/19.
//  Copyright © 2019 Aaron Graubert. All rights reserved.
//

#include "scrinvex.h"
#include <stdio.h>
#include <memory>
#include <args.hxx>
#include <boost/filesystem.hpp>
#include <Metrics.h>
#include <Expression.h>

using namespace args;
using namespace std;
using namespace scrinvex;

namespace scrinvex {
    unsigned int missingBC, missingUMI, skippedBC;
    unordered_map<string, unsigned long> intergenicCounts;
}

int main(int argc, char* argv[])
{
    ArgumentParser parser("SCRINVEX - A Single Cell RNA-Seq QC tool");
    HelpFlag help(parser, "help", "Display this message and quit", {'h', "help"});
    Positional<string> gtfFile(parser, "gtf", "The input GTF file containing features to check the bam against");
    Positional<string> bamFile(parser, "bam", "The input SAM/BAM file containing reads to process");
    ValueFlag<string> outputPath(parser, "ouput", "Path to output file.  Default: {current directory}/{bam filename}.scrinvex.tsv", {'o', "output"});
    ValueFlag<string> barcodeFile(parser, "barcodes", "Path to filtered barcodes.tsv file from cellranger. Only barcodes listed in the file will be used. Default: All barcodes present in bam", {'b', "barcodes"});
    ValueFlag<unsigned int> mappingQualityThreshold(parser,"quality", "Set the lower bound on read quality for coverage counting. Reads below this quality are skipped. Default: 255", {'q', "quality"});
    ImplicitValueFlag<string> summaryFile(parser, "path", "Produce a summary of counts by barcode in a separate file. This includes a count of intergenic reads. If the flag is provided with no arguments, this defaults to {current directory}/{bam filename}.scrinvex.summary.tsv. You may provide a different path as an argument to this flag", {'s', "summary"}, "", "");
    try
    {
        parser.ParseCLI(argc, argv);

        if (!gtfFile) throw ValidationError("No GTF file provided");
        if (!bamFile) throw ValidationError("No BAM file provided");

        const string OUTPUTPATH = outputPath ? outputPath.Get() : (boost::filesystem::path(bamFile.Get()).filename().string() + ".scrinvex.tsv");
        const unsigned int MAPQ = mappingQualityThreshold ? mappingQualityThreshold.Get() : 255u;
        const string SUMMARYPATH = summaryFile.Get().empty() ? (boost::filesystem::path(bamFile.Get()).filename().string() + ".scrinvex.summary.tsv") : summaryFile.Get();
        InvexCounter summaryCounts;
        InvexCounter* const SUMMARYPTR = summaryFile ? &summaryCounts : nullptr;
        
        Feature line; //current feature being read from the gtf
        ifstream reader(gtfFile.Get());
        if (!reader.is_open())
        {
            cerr << "Unable to open GTF file: " << gtfFile.Get() << endl;
            return 10;
        }

        cout << "                                        \033[1;31m                \033[0m                   ***                         " << endl;
        cout << "                       ***              \033[1;31m                \033[0m                *****                         " << endl;
        cout << "                     *****              \033[1;31m     ***        \033[0m *            *****                           " << endl;
        cout << "              **   *****  ***********   \033[1;31m  * ******      \033[0m **          ****                             " << endl;
        cout << "            ***** ****    ***********   \033[1;31m ** ********    \033[0m ***         ***    ****        ***        " << endl;
        cout << "         ******   ***     ***  *****    \033[1;31m*** ***  *****  \033[0m ***     **  ***    ******    *****        " << endl;
        cout << "       ******     ***     ********      \033[1;31m*** ***    *****\033[0m ***     *** *******  ***********          " << endl;
        cout << "     ******       ***     *******       \033[1;31m*** ***      ***\033[0m ***     *** *******    *******            " << endl;
        cout << "   ************** ***     *********     \033[1;31m*** ***      ***\033[0m ***     *** ***       *********           " << endl;
        cout << " **************** ***     ***   *****   \033[1;31m*** ***      ***\033[0m ***     *** ***      *****  *****         " << endl;
        cout << "           *****  *****   ***     ***** \033[1;31m*** ***      ***\033[0m ***     *** ***     ***       *****       " << endl;
        cout << "         *****      ***** ***       *** \033[1;31m*** ***      ***\033[0m ***    **** *****  **           *****     " << endl;
        cout << "       *****          *****             \033[1;31m*** ***      ***\033[0m ***  *****    *****               *****   " << endl;
        cout << "     *****              ****            \033[1;31m    ***         \033[0m ********        *****               ****  " << endl;
        cout << "    ****                  ***           \033[1;31m    **          \033[0m ******            *****               *** " << endl;
        cout << "                            **          \033[1;31m                \033[0m ****                ***                 **" << endl;
        cout << "                                        \033[1;31m                \033[0m **                                        " << endl;

        unordered_set<string> goodBarcodes;
        if (barcodeFile)
        {
            cout << "Reading barcodes" << endl;
            ifstream barcodeReader(barcodeFile.Get());
            if (!barcodeReader.is_open())
            {
                cerr << "Unable to open barcodes file: " << barcodeFile.Get() << endl;
                return 10;
            }
            string barcode;
            while (barcodeReader >> barcode) goodBarcodes.insert(barcode);
            cout << "Filtering input using " << goodBarcodes.size() << " barcodes" << endl;
        }

        cout << "Parsing GTF" << endl;

        unsigned long featcnt = 0, alignmentCount = 0;
        map<chrom, list<Feature>> features;
        while (reader >> line)
        {
            // Only record Genes and Exons. Transcripts not important for scrinvex
            if (line.type == FeatureType::Gene || line.type == FeatureType::Exon)
            {
                features[line.chromosome].push_back(line);
                ++featcnt;
            }
        }
        // Sort all features by position
        for (auto entry : features)
            entry.second.sort(compIntervalStart);
        cout << featcnt << " features loaded" << endl;

        geneCounters counts; //barcode -> counts

        SeqlibReader bam;
        if (!bam.open(bamFile.Get()))
        {
            cerr << "Unable to open BAM file: " << bamFile.Get() << endl;
            return 10;
        }

        // Get the list of contigs present in bam header
        SeqLib::BamHeader header = bam.getHeader();
        SeqLib::HeaderSequenceVector sequences = header.GetHeaderSequenceVector();

        // Intersect bam header with gtf contigs to make sure they share the same naming scheme
        bool hasOverlap = false;
        for(auto sequence : sequences)
        {
            chrom chrom = chromosomeMap(sequence.Name);
            if (features.find(chrom) != features.end())
            {
                hasOverlap = true;
                break;
            }
        }
        if (!hasOverlap)
        {
            cerr << "BAM file shares no contigs with GTF" << endl;
            return 11;
        }

        Alignment alignment;

        int32_t last_position = 0; // For some reason, htslib has decided that this will be the datatype used for positions
        chrom current_chrom = 0;

        // Open all output files
        ofstream output(OUTPUTPATH);
        output << "gene_id\tbarcode\tintrons\tjunctions\texons\tsense\tantisense" << endl;
        
        cout << "Parsing BAM" << endl;

        while (bam.next(alignment))
        {
            ++alignmentCount;
            // Only consider uniquely mapped reads
            if (!(alignment.SecondaryFlag() || alignment.QCFailFlag()) && alignment.MappedFlag() && alignment.MapQuality() >= MAPQ)
            {
                chrom chr = getChrom(alignment, sequences); //parse out a chromosome shorthand
                if (chr != current_chrom)
                {
                    // If we've switched chromosomes, drop all features from that chromosome
                    // Saves memory and also writes out the coverage data
                    dropFeatures(features[current_chrom], counts, output);
                    current_chrom = chr;
                }
                else if (last_position > alignment.Position())
                    cerr << "Warning: The input bam does not appear to be sorted. An unsorted bam will yield incorrect results" << endl;
                last_position = alignment.Position();
                trimFeatures(alignment, features[chr], counts, output); //drop features that appear before this read
                countRead(counts, features[chr], alignment, chr, goodBarcodes, SUMMARYPTR);
            }
        }

        cout << "Finalizing data" << endl;
        // Drop all remaining genes to ensure their coverage data has been written
        for (auto contig : features) if (contig.second.size()) dropFeatures(contig.second, counts, output);
        output.close();
        
        if (summaryFile)
        {
            ofstream summary(SUMMARYPATH);
            summary << "barcode\tintrons\tjunctions\texons\tsense\tantisense\tintergenic" << endl;
            set<string> barcodes;
            for (const string &barcode : summaryCounts.getBarcodes(barcodes))
            {
                countTuple &data = summaryCounts.getCounts(barcode);
                auto i = get<INTRONS>(data), j = get<JUNCTIONS>(data), e = get<EXONS>(data);
                auto s = get<SENSE>(data), a = get<ANTISENSE>(data);
                unsigned long n = intergenicCounts[barcode];
                if (i + j + e + s + a + n > 0)
                {
                    summary << barcode << "\t" << i;
                    summary << "\t" << j << "\t" << e << "\t" << s << "\t" << a << "\t" << n << endl;
                }
            }
            summary.close();
        }
        
        if (missingUMI + missingBC)
            cerr << "There were " << missingBC << " reads without a barcode (CB) and " << missingUMI << " reads without a UMI (UB)" << endl;

        if (skippedBC)
            cerr << "Skipped " << skippedBC << " reads with barcodes not listed in " << barcodeFile.Get() << endl;

        return 0;
    }
    catch (args::Help)
    {
        cout << parser;
        return 4;
    }
    catch (args::ParseError &e)
    {
        cerr << parser << endl;
        cerr << "Argument parsing error: " << e.what() << endl;
        return 5;
    }
    catch (args::ValidationError &e)
    {
        cerr << parser << endl;
        cerr << "Argument validation error: " << e.what() << endl;
        return 6;
    }
    catch (fileException &e)
    {
        cerr << e.error << endl;
        return 10;
    }
    catch (invalidContigException &e)
    {
        cerr << "GTF referenced a contig not present in the FASTA: " << e.error << endl;
        return 11;
    }
    catch (gtfException &e)
    {
        cerr << "Failed to parse the GTF: " << e.error << endl;
        return 11;
    }
    catch (boost::filesystem::filesystem_error &e)
    {
        cerr << "Filesystem error:  " << e.what() << endl;
        return 8;
    }
    catch(ios_base::failure &e)
    {
        cerr << "Encountered an IO failure" << endl;
        cerr << e.what() << endl;
        return 10;
    }
    catch(std::bad_alloc &e)
    {
        cerr << "Memory allocation failure. Out of memory" << endl;
        cerr << e.what() << endl;
        return 10;
    }
    catch (...)
    {
        cerr << parser << endl;
        cerr << "Unknown error" << endl;
        return -1;
    }
}

namespace scrinvex {

    countTuple& InvexCounter::getCounts(const std::string &barcode)
    {
        return this->counts[barcode];
    }

    set<string>& InvexCounter::getBarcodes(set<string> &destination) const
    {
        // Insert all barcodes from this InvexCounter into a given set
        for (auto entry : this->counts) destination.insert(entry.first);
        return destination;
    }
    
    inline void updateCounts(unsigned int genicLength, unsigned int exonicLength, countTuple &counts, const bool sense)
    {
        if (genicLength > exonicLength)
        {
            if (exonicLength) get<JUNCTIONS>(counts) += 1; // read aligned to some exons and introns
            else get<INTRONS>(counts) += 1; // read aligned to all introns
        }
        else get<EXONS>(counts) += 1; // read aligned to entirely exons
        if (sense) get<SENSE>(counts) += 1; // read aligned to sense strand
        else get<ANTISENSE>(counts) += 1;
    }

    void countRead(geneCounters &counts, std::list<Feature> &features, Alignment &alignment, chrom chromosome, const std::unordered_set<std::string> &goodBarcodes, InvexCounter *summary)
    {
        // We don't expect goodBarcodes to change, so just grab its size once
        static const size_t n_barcodes = goodBarcodes.size();

        // Now parse the Cigar string to get the set of intervals that this read aligns over
        vector<Feature> alignedSegments;
        extractBlocks(alignment, alignedSegments, chromosome, false);

        alignmentLengthTracker lengths;

        // Extract the barcode and umi. Check that they're present and barcode is in the set of good barcodes
        string barcode, umi;
        if (!alignment.GetZTag(BARCODE_TAG, barcode))
        {
            ++missingBC;
            return;
        }
        if (!alignment.GetZTag(UMI_TAG, umi))
        {
            ++missingUMI;
            return;
        }
        if (n_barcodes > 0 && goodBarcodes.count(barcode) == 0)
        {
            ++skippedBC;
            return;
        }
        
        unordered_map<string, bool> sense_antisense;

        // Intersect all aligned segments with the list of features.
        for (Feature &segment : alignedSegments)
        {
            shared_ptr<list<Feature> > intersections = shared_ptr<list<Feature> >(intersectBlock(segment, features));
            for (Feature &genomeFeature : *intersections)
            {
                // Count the total number of read bases which align to genes and exons
                if (genomeFeature.type == FeatureType::Exon && fragmentTracker[genomeFeature.gene_id].count(umi) == 0)
                    get<EXONIC_ALIGNED_LENGTH>(lengths[genomeFeature.gene_id]) += partialIntersect(genomeFeature, segment);
                else if (genomeFeature.type == FeatureType::Gene && fragmentTracker[genomeFeature.feature_id].count(umi) == 0) {
                    get<GENIC_ALIGNED_LENGTH>(lengths[genomeFeature.gene_id]) += partialIntersect(genomeFeature, segment);
                    sense_antisense.emplace(genomeFeature.gene_id, genomeFeature.strand == segment.strand);
                }
            }
        }
        unsigned long totalGenicLength = 0;
        // For every gene that this read aligned to
        for (auto entry : lengths)
        {
            unsigned int genicLength = get<GENIC_ALIGNED_LENGTH>(entry.second), exonicLength = get<EXONIC_ALIGNED_LENGTH>(entry.second);
            if (genicLength > 0)
            {
                totalGenicLength += genicLength;
                updateCounts(genicLength, exonicLength, counts[entry.first].getCounts(barcode), sense_antisense[entry.first]);
                if (summary != nullptr) updateCounts(genicLength, exonicLength, summary->getCounts(barcode), sense_antisense[entry.first]);

                // Now add the UMI to the tracker so we skip UMI duplicates
                fragmentTracker[entry.first].insert(umi);
            }
        }
        
        if (totalGenicLength == 0ul) intergenicCounts[barcode] += 1;
    }

    chrom getChrom(Alignment &alignment, SeqLib::HeaderSequenceVector &sequences)
    {
        return chromosomeMap(sequences[alignment.ChrID()].Name);
    }
    
    void writeFeature(const string &gene_id, InvexCounter &invex, ostream &output)
    {
        set<string> barcodes;
        for (const string &barcode : invex.getBarcodes(barcodes))
        {
            countTuple &data = invex.getCounts(barcode);
            auto i = get<INTRONS>(data), j = get<JUNCTIONS>(data), e = get<EXONS>(data);
            auto s = get<SENSE>(data), a = get<ANTISENSE>(data);
            if (i + j + e + s + a > 0)
            {
                output << gene_id << "\t" << barcode << "\t" << i;
                output << "\t" << j << "\t" << e << "\t" << s << "\t" << a << endl;
            }
        }
    }

    void dropFeatures(std::list<Feature> &features, geneCounters &counts, std::ostream &output)
    {
        for (Feature &feat : features) if (feat.type == FeatureType::Gene) {
            // For all genes, dump their coverage data
            fragmentTracker.erase(feat.feature_id);
            InvexCounter &invex = counts[feat.feature_id];
            writeFeature(feat.feature_id, invex, output);
        }
        features.clear();
    }
    
    void trimFeatures(Alignment &alignment, std::list<Feature> &features, geneCounters &counts, std::ostream &output)
    {
        auto cursor = features.begin();
        while (cursor != features.end() && cursor->end < alignment.Position())
        {
            if (cursor->type == FeatureType::Gene) {
                // For all genes, dump their coverage data
                fragmentTracker.erase(cursor->feature_id);
                InvexCounter &invex = counts[cursor->feature_id];
                writeFeature(cursor->gene_id, invex, output);
            }
            ++cursor;
        }
        // Also erase all features up to (but not including) the cursor. They are now outside the search window
        // This saves memory and greatly improves runtime
        features.erase(features.begin(), cursor);
    }
}
