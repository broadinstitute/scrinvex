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
#include <Metrics.h>
#include <Expression.h>

using namespace args;
using namespace std;

int main(int argc, char* argv[])
{
    ArgumentParser parser("SCRINVEX");
    HelpFlag help(parser, "help", "Display this message and quit", {'h', "help"});
    Positional<string> gtfFile(parser, "gtf", "The input GTF file containing features to check the bam against");
    Positional<string> bamFile(parser, "bam", "The input SAM/BAM file containing reads to process");
    Positional<string> outputPath(parser, "output", "Output filepath. Defaults to stdout");
    try
    {
        parser.ParseCLI(argc, argv);

        if (!gtfFile) throw ValidationError("No GTF file provided");
        if (!bamFile) throw ValidationError("No BAM file provided");
        if (!outputPath) cerr << "Writing output to stdout" << endl;

        Feature line; //current feature being read from the gtf
        ifstream reader(gtfFile.Get());
        if (!reader.is_open())
        {
            cerr << "Unable to open GTF file: " << gtfFile.Get() << endl;
            return 10;
        }

        unsigned long featcnt = 0, alignmentCount = 0;
        map<chrom, list<Feature>> features;
        while (reader >> line)
        {
            if (line.type == FeatureType::Gene || line.type == FeatureType::Exon)
            {
                features[line.chromosome].push_back(line);
                ++featcnt;
            }
        }
        for (auto entry : features)
            entry.second.sort(compIntervalStart);
        cerr << featcnt << " features loaded" << endl;

        geneCounters counts; //barcode -> counts

        {
            SeqlibReader bam;
            if (!bam.open(bamFile.Get()))
            {
                cerr << "Unable to open BAM file: " << bamFile.Get() << endl;
                return 10;
            }
            SeqLib::BamHeader header = bam.getHeader();
            SeqLib::HeaderSequenceVector sequences = header.GetHeaderSequenceVector();

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

            while (bam.next(alignment))
            {
                ++alignmentCount;
                if (!(alignment.SecondaryFlag() || alignment.QCFailFlag()) && alignment.MappedFlag())
                {
                    chrom chr = getChrom(alignment, sequences); //parse out a chromosome shorthand
                    if (chr != current_chrom)
                    {
                        dropFeatures(features[current_chrom]);
                        current_chrom = chr;
                    }
                    else if (last_position > alignment.Position())
                        cerr << "Warning: The input bam does not appear to be sorted. An unsorted bam will yield incorrect results" << endl;
                    last_position = alignment.Position();
                    trimFeatures(alignment, features[chr]); //drop features that appear before this read
                    string barcode;
                    alignment.GetZTag(BARCODE_TAG, barcode);
                    counts[barcode].countRead(features[chr], alignment, chr);
                }
            }
        }

        cerr << "Generating Report" << endl;
        
        if (outputPath)
        {
            ofstream report(outputPath.Get());
            report << counts;
            report.close();
        }
        else cout << counts;

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

void dropFeatures(std::list<Feature> &features)
{
    for (Feature &feat : features) if (feat.type == FeatureType::Gene) fragmentTracker.erase(feat.feature_id);
    features.clear();
}

void InvexCounter::countRead(std::list<Feature> &features, Alignment &alignment, chrom chromosome)
{
    vector<Feature> alignedSegments;
    extractBlocks(alignment, alignedSegments, chromosome, false);
    alignmentLengthTracker lengths;
    string umi;
    alignment.GetZTag(UMI_TAG, umi);
    for (Feature &segment : alignedSegments)
    {
        shared_ptr<list<Feature> > intersections = shared_ptr<list<Feature> >(intersectBlock(segment, features));
        for (Feature &genomeFeature : *intersections)
        {
            if (genomeFeature.type == FeatureType::Exon && fragmentTracker[genomeFeature.gene_id].count(umi) == 0)
                get<EXONIC_ALIGNED_LENGTH>(lengths[genomeFeature.gene_id]) += partialIntersect(genomeFeature, segment);
            else if (genomeFeature.type == FeatureType::Gene && fragmentTracker[genomeFeature.feature_id].count(umi) == 0)
                get<GENIC_ALIGNED_LENGTH>(lengths[genomeFeature.gene_id]) += partialIntersect(genomeFeature, segment);
        }
    }
    for (auto entry : lengths)
    {
        unsigned int genicLength = get<GENIC_ALIGNED_LENGTH>(entry.second), exonicLength = get<EXONIC_ALIGNED_LENGTH>(entry.second);
        if (genicLength > 0)
        {
            if (genicLength > exonicLength)
            {
                if (exonicLength) ++(this->junctions);
                else ++(this->introns);
            }
            else ++(this->exons);
            fragmentTracker[entry.first].insert(umi);
        }
    }
}

chrom getChrom(Alignment &alignment, SeqLib::HeaderSequenceVector &sequences)
{
    return chromosomeMap(sequences[alignment.ChrID()].Name);
}

ostream& operator<<(ostream &stream, const InvexCounter &counter)
{
    stream << counter.introns << "\t" << counter.junctions << "\t" << counter.exons;
    return stream;
}

std::ostream& operator<<(std::ostream &stream, const geneCounters &counters)
{
    stream << "Barcode\tIntrons\tJunctions\tExons" << endl;
    for (auto entry : counters)
    {
        stream << entry.first << "\t";
        stream << entry.second << endl;
    }
    return stream;
}
