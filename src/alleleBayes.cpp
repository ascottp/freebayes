// 
// *Bayes
//
// A bayesian genetic variant parser.
// 

// standard includes
//#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <float.h>

// "boost" regular expression library
#include <boost/regex.hpp>

// "boost" string manipulation
#include <boost/algorithm/string/join.hpp>
// tuple
#include <boost/tuple/tuple.hpp>
// bind
#include <boost/bind.hpp>

// private libraries
#include "BamReader.h"
#include "Fasta.h"
#include "TryCatch.h"
#include "Parameters.h"
#include "Allele.h"
#include "AlleleParser.h"
#include "Utility.h"

#include "multichoose.h"
#include "multipermute.h"

#include "Genotype.h"
#include "GenotypePriors.h"
#include "DataLikelihood.h"
#include "ResultData.h"

//#include <boost/foreach.hpp>

#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>


// local helper debugging macros to improve code readability
#define DEBUG(msg) \
    if (parameters.debug) { cerr << msg << endl; }

// lower-priority messages
#define DEBUG2(msg) \
    if (parameters.debug2) { cerr << msg << endl; }

// must-see error messages
#define ERROR(msg) \
    cerr << msg << endl;


using namespace std; 

using boost::tuple;
using boost::make_tuple;

// Allele object recycling:
//
// We use the Allele freelist for performance reasons.  When an Allele object
// is destroyed, it is pushed onto this freelist.  When a new Allele object is
// created, new first checks if we have a free Allele object on the freelist.
// Because we are dynamically linked, we have to declare the freelist here,
// although it exists as a static member of the Allele class.
//
AlleleFreeList Allele::_freeList;


int main (int argc, char *argv[]) {

    AlleleParser* parser = new AlleleParser(argc, argv);
    Parameters& parameters = parser->parameters;
    list<Allele*> alleles;

    ostream& out = *(parser->output);
    ofstream trace;
    trace.open("allelebayes.trace");

    // this can be uncommented to force operation on a specific set of genotypes
    vector<Allele> allGenotypeAlleles;
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "A", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "T", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "G", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "C", 1));
    //vector<Genotype> genotypes = allPossibleGenotypes(parameters.ploidy, genotypeAlleles);

    vector<AlleleType> allowedAlleles;
    allowedAlleles.push_back(ALLELE_REFERENCE);
    allowedAlleles.push_back(ALLELE_SNP);
    //allowedAlleles.push_back(ALLELE_INSERTION);
    //allowedAlleles.push_back(ALLELE_DELETION);

    // output VCF header
    if (parameters.output == "vcf") {
        vcfHeader(out, parser->reference->filename, parser->sampleList);
    }

    while (parser->getNextAlleles(alleles)) {

        DEBUG2("at start of main loop");

        filterAlleles(alleles, allowedAlleles);
        
        DEBUG2("alleles filtered");

        map<string, vector<Allele*> > sampleGroups1 = groupAllelesBySample(alleles);
        for (map<string, vector<Allele*> >::iterator g = sampleGroups1.begin(); g != sampleGroups1.end(); ++g) {
            vector<Allele*>& group = g->second;
            for (vector<Allele*>::iterator a = group.begin(); a != group.end(); ++a) {
                Allele& allele = **a;
                trace << parser->currentTarget->seq << "," << parser->currentPosition + 1 << ","
                    << g->first << "," << allele.readID << "," << allele.base() << "," 
                    << allele.currentQuality() << "," << allele.mapQuality << endl;
            }
        }
        DEBUG2("after trace generation");

        // skips 0-coverage regions
        if (alleles.size() == 0) {
            //cerr << "no alleles found at " << parser->currentTarget->seq << ":" << parser->currentPosition << endl;
            DEBUG2("no alleles left at this site after filtering");
            continue;
        }

        DEBUG2("coverage " << parser->currentTarget->seq << ":" << parser->currentPosition << " == " << alleles.size());

        // establish a set of possible alternate alleles to evaluate at this location
        // only evaluate alleles with at least one supporting read with mapping
        // quality (MQL1) and base quality (BQL1)

        vector<Allele> genotypeAlleles;

        vector<vector<Allele*> > alleleGroups = groupAlleles(alleles, &allelesEquivalent);
        DEBUG2("grouped alleles by equivalence");

        map<string, vector<Allele*> > sampleGroups = groupAllelesBySample(alleles);
        DEBUG2("grouped alleles by sample");

        for (vector<vector<Allele*> >::iterator group = alleleGroups.begin(); group != alleleGroups.end(); ++group) {
            // for each allele that we're going to evaluate, we have to have at least one supporting read with
            // map quality >= MQL1 and the specific quality of the allele has to be >= BQL1
            for (vector<Allele*>::iterator a = group->begin(); a != group->end(); ++a) {
                Allele& allele = **a;
                if (allele.mapQuality >= parameters.MQL1 && allele.currentQuality() >= parameters.BQL1) {
                    int length = (allele.type == ALLELE_REFERENCE || allele.type == ALLELE_SNP) ? 1 : allele.length;
                    genotypeAlleles.push_back(genotypeAllele(allele.type, allele.base(), length));
                    break;
                }
            }
        }
        DEBUG2("found genotype alleles");

        vector<Allele> filteredGenotypeAlleles;

        // remove genotypeAlleles for which we don't have any individuals with sufficient alternate observations
        for (vector<Allele>::iterator genotypeAllele = genotypeAlleles.begin();
                genotypeAllele != genotypeAlleles.end(); ++genotypeAllele) {

            for (map<string, vector<Allele*> >::iterator sample = sampleGroups.begin();
                    sample != sampleGroups.end(); ++sample) {

                vector<Allele*>& observedAlleles = sample->second;
                int alleleCount = 0;
                for (vector<Allele*>::iterator a = observedAlleles.begin(); a != observedAlleles.end(); ++a) {
                    if (**a == *genotypeAllele)
                        ++alleleCount;
                }
                if (alleleCount >= parameters.minAltCount 
                        && ((float) alleleCount / (float) observedAlleles.size()) >= parameters.minAltFraction) {
                    //cerr << *genotypeAllele << " has support of " << alleleCount 
                    //    << " in individual " << sample->first << " and fraction " 
                    //    << (float) alleleCount / (float) observedAlleles.size() << endl;
                    filteredGenotypeAlleles.push_back(*genotypeAllele);
                    //out << *genotypeAllele << endl;
                    goto hasSufficient;
                }
            }
            hasSufficient: ;
        }

        DEBUG2("filtered genotype alleles");

        if (parameters.useAllGenotypes) {
            DEBUG2("using all genotypes, adding alleles to filteredGenotypeAlleles");
            for (vector<Allele>::iterator a = allGenotypeAlleles.begin(); a != allGenotypeAlleles.end(); ++a) {
                bool hasAllele = false;
                for (vector<Allele>::iterator f = filteredGenotypeAlleles.begin(); f != filteredGenotypeAlleles.end(); ++f) {
                    if (*f == *a) {
                        hasAllele = true;
                        break;
                    }
                }
                if (!hasAllele) {
                    DEBUG2("adding allele to filteredGenotypeAlleles " << *a);
                    filteredGenotypeAlleles.push_back(*a);
                }
            }
        }

        if (filteredGenotypeAlleles.size() <= 1) { // if we have only one viable alternate, we don't have evidence for variation at this site
            DEBUG2("no alternate genotype alleles passed filters at " << parser->currentTarget->seq << ":" << parser->currentPosition);
            continue;
        }
        DEBUG2("genotype alleles: " << filteredGenotypeAlleles);

        //cerr << filteredGenotypeAlleles << endl;

        vector<Genotype> genotypes = allPossibleGenotypes(parameters.ploidy, filteredGenotypeAlleles);
        DEBUG2("generated all possible genotypes");

        // continue in the case that no individual has more than some
        // fraction of alternate allele observations ...  (default is 0.1)
        //if (!sufficientAlternateObservations(sampleGroups, parameters.minAltCount, parameters.minAltFraction))
         //   continue;

        Results results;

        DEBUG2("calculating data likelihoods");
        // calculate data likelihoods
        for (map<string, vector<Allele*> >::iterator sample = sampleGroups.begin();
                sample != sampleGroups.end(); ++sample) {

            string sampleName = sample->first;
            vector<Allele*>& observedAlleles = sample->second;

            vector<pair<Genotype*, long double> > probs = 
                probObservedAllelesGivenGenotypes(observedAlleles, genotypes);

            /*
            vector<pair<Genotype*, long double> > approxprobs = 
                approxProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);
            vector<pair<Genotype*, long double> > bambayesapproxprobs = 
                bamBayesApproxProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);
            //vector<pair<Genotype*, long double> > exactprobs = 
                //exactProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);

            cout << sampleName << endl;
            for (vector<Allele*>::iterator i = observedAlleles.begin(); i != observedAlleles.end(); ++i) {
                Allele& allele = **i;
                cout << allele.base() << " ";
            }
            cout << endl;
            vector<pair<Genotype*, long double> >::iterator d = approxprobs.begin();
            vector<pair<Genotype*, long double> >::iterator b = bambayesapproxprobs.begin();
            //vector<pair<Genotype*, long double> >::iterator e = exactprobs.begin();
            for ( ; d != approxprobs.end() && b != bambayesapproxprobs.end(); ++d, ++b) {
                //cout << "exact  : " << *e->first << " " << e->second << endl;
                cout << "bapprox: " << *b->first << " " << b->second << endl;
                cout << "dapprox: " << *d->first << " " << d->second << endl;
                cout << endl;
            }
            */
            
            map<Genotype*, long double> marginals;
            map<Genotype*, vector<long double> > rawMarginals;

            results.insert(make_pair(sampleName, ResultData(sampleName, probs, marginals, rawMarginals, observedAlleles)));

        }
        
        DEBUG2("finished calculating data likelihoods");

        // sort genotype data likelihoods and accumulate into 
        
        vector<pair<string, vector<pair<Genotype*, long double> > > > sampleGenotypes;
        for (Results::iterator s = results.begin(); s != results.end(); ++s) {
            s->second.sortDataLikelihoods();
            /*
            for (vector<pair<Genotype, long double> >::iterator p = s->second.dataLikelihoods.begin(); p != s->second.dataLikelihoods.end(); ++p) {
                cout << p->first << " " << p->second << endl;
            }
            cout << endl;
            */

            sampleGenotypes.push_back(make_pair(s->first, s->second.dataLikelihoods));
        }

        DEBUG2("finished sorting data likelihoods");

        // calculate genotype combo likelihoods, integral over nearby genotypes
        // calculate marginals
        // and determine best genotype combination

        // TODO add in all-alternate and all-reference
        // ... or at least very add one which is consistent
        //vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinations(sampleGenotypes, 2, 2);
        //vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinationsIncludingBestHomozygousCombo(sampleGenotypes, 2, 2);
        DEBUG2("generating banded genotype combinations");
        vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinationsIncludingAllHomozygousCombos(sampleGenotypes, genotypes, 2, 2);
        DEBUG2("...done");

        vector<pair<GenotypeCombo, long double> > genotypeComboProbs;

        DEBUG2("calculating genotype combination likelihoods");
        for (vector<GenotypeCombo>::iterator combo = bandedCombos.begin(); combo != bandedCombos.end(); ++combo) {

            long double probabilityObservationsGivenGenotypes = 0;
            vector<Genotype*> genotypeCombo;

            for (GenotypeCombo::iterator i = combo->begin(); i != combo->end(); ++i) {
                genotypeCombo.push_back(i->second.first);
                probabilityObservationsGivenGenotypes += i->second.second;
            }

            /*
            map<Allele, int> alleleCounts = countAlleles(genotypeCombo);
            cout << "alleleCounts = ";
            for (map<Allele, int>::iterator a = alleleCounts.begin(); a != alleleCounts.end(); ++a) {
                cout << " ( " << a->first.base() << " " << a->second << ")";
            }
            cout << endl;

            map<int, int> frequencyCounts = countFrequencies(genotypeCombo);
            */

            long double priorProbabilityOfGenotypeCombo = alleleFrequencyProbabilityln(countFrequencies(genotypeCombo), parameters.TH);
            //cout << "priorProbabilityOfGenotypeCombo = " << priorProbabilityOfGenotypeCombo << endl;
            //cout << "probabilityObservationsGivenGenotypes = " << probabilityObservationsGivenGenotypes << endl;
            long double comboProb = priorProbabilityOfGenotypeCombo + probabilityObservationsGivenGenotypes;

            // XXX hack to prevent underflow
            //if (comboProb < DBL_MIN_EXP)
                //continue;

            for (GenotypeCombo::iterator i = combo->begin(); i != combo->end(); ++i) {
                map<Genotype*, vector<long double> >& marginals = results[i->first].rawMarginals;
                Genotype* genotype = i->second.first;
                //long double& prob = i->second.second;
                map<Genotype*, vector<long double> >::iterator marginal = marginals.find(genotype);
                if (marginal == marginals.end()) {
                    vector<long double> probs;
                    probs.push_back(comboProb);
                    marginals.insert(make_pair(genotype, probs));
                } else {
                    marginals[genotype].push_back(comboProb);
                }
            }

            genotypeComboProbs.push_back(make_pair(*combo, comboProb));

        }

        // XXX AWFUL hack, but necessary to guard against the case that all our genotype probabilites are <-200 log
        //if (genotypeComboProbs.size() == 0)
            //continue;
    
        DEBUG2("finished calculating genotype combination likelihoods");

        // genotype_combo_probs = sorted(genotype_combo_probs, key=lambda c: c[1], reverse=True)
        sort(genotypeComboProbs.begin(), genotypeComboProbs.end(),
                boost::bind(&pair<GenotypeCombo, long double>::second, _1) 
                    > boost::bind(&pair<GenotypeCombo, long double>::second, _2));

        DEBUG2("sorted genotype combination likelihoods");

        // get posterior normalizer
        vector<long double> comboProbs;
        comboProbs.resize(genotypeComboProbs.size());
        transform(genotypeComboProbs.begin(), genotypeComboProbs.end(),
                comboProbs.begin(), boost::bind(&pair<GenotypeCombo, long double>::second, _1));
        long double posteriorNormalizer = logsumexp_probs(comboProbs);
        DEBUG2("got posterior normalizer");

        /*
        for (vector<pair<GenotypeCombo, long double> >::iterator c = genotypeComboProbs.begin(); c != genotypeComboProbs.end(); ++c) {
            cout << "prob:" << c->second << endl;
        }
        */
        //cout << "posteriorNormalizer = " << posteriorNormalizer << endl;
        /*
        cout << "comboProbs = [";
        for (vector<long double>::iterator i = comboProbs.begin(); i != comboProbs.end(); ++i)
            cout << *i << " ";
        cout << "]" << endl;
        */
        

        // TODO Gabor had a suggestion about this normalization, that we only normalize 'perturbed' cases
        // ....  but i think that this thought was not asserted as necessary
        // normalize marginals
        for (Results::iterator r = results.begin(); r != results.end(); ++r) {
            ResultData& d = r->second;
            //cout << r->first << endl;
            for (map<Genotype*, vector<long double> >::iterator m = d.rawMarginals.begin(); m != d.rawMarginals.end(); ++m) {
                /*
                cout << *m->first << " rawMarginals = [";
                for (vector<long double>::iterator i = m->second.begin(); i != m->second.end(); ++i)
                    cout << *i << ", ";
                cout << "]" << endl;
                cout << logsumexp_probs(m->second) << endl;
                */
                d.marginals[m->first] = logsumexp_probs(m->second) - posteriorNormalizer;
            }
        }
        DEBUG2("calculated marginal likelihoods");

        // we provide p(var|data), or the probability that the location has
        // variation between individuals relative to the probability that it
        // has no variation
        //
        // in other words:
        // p(var|d) = 1 - p(AA|d) - p(TT|d) - P(GG|d) - P(CC|d)
        //
        // the approach is go through all the homozygous combos
        // and then subtract this from 1... resolving p(var|d)

        long double pVar = 1.0;

        for (vector<pair<GenotypeCombo, long double> >::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
            /*
            for (GenotypeCombo::iterator c = gc->first.begin(); c != gc->first.end(); ++c)
                cout << *c->second.first << " ";
            cout << endl;
            cout << (long double) gc->second << endl;
            */
            if (isHomozygousCombo(gc->first)) {
                pVar -= safe_exp(gc->second - posteriorNormalizer);
            }
        }
        DEBUG2("calculated pVar");

        // TODO as we only ever report heterozygous combos, we must get the best non-homozygous case here...
        // .... even if the best case is fully homozygous
        //GenotypeCombo& bestGenotypeCombo = genotypeComboProbs.front().first;
        //long double bestGenotypeComboProb = safe_exp(genotypeComboProbs.front().second - posteriorNormalizer);
        //cout << bestGenotypeComboProb << endl;

        GenotypeCombo* besthc = NULL;
        long double bestHetGenotypeComboProb;
        // these are sorted according to probability, so we just step through until we find the first het
        for (vector<pair<GenotypeCombo, long double> >::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
            if (!isHomozygousCombo(gc->first)) {
                besthc = &(gc->first);
                bestHetGenotypeComboProb = safe_exp(gc->second - posteriorNormalizer);
                break;
            }
        }
        if (besthc == NULL) {
            ERROR("could not find a heterozygous genotype combo at " << parser->currentTarget->seq << ":" << parser->currentPosition);
            continue; // just raise error and continue
        }
        GenotypeCombo& bestHetGenotypeCombo = *besthc;
        vector<Genotype*> bestComboGenotypes;
        for (GenotypeCombo::iterator g = bestHetGenotypeCombo.begin(); g != bestHetGenotypeCombo.end(); ++g)
            bestComboGenotypes.push_back(g->second.first);
        long double bestHetComboAlleleSamplingProb = safe_exp(alleleFrequencyProbabilityln(countFrequencies(bestComboGenotypes), parameters.TH));


        DEBUG2("got bestAlleleSamplingProb");
        DEBUG2("pVar = " << pVar << " " << parameters.PVL);

        if (!parameters.suppressOutput) {
            //cerr << parser->currentPosition << " " << alleles.size() << " " << bestGenotypeComboProb << " " << genotypeComboProbs.front().second << " " <<  posteriorNormalizer << endl;

            if (parameters.output == "json") {
                out << "{ \"position\": " << parser->currentPosition + 1 // 1-based reporting, to match vcf
                    << ", \"sequence\": " << parser->currentTarget->seq
                    << ", \"best_genotype_combo\":" << bestHetGenotypeCombo
                    << ", \"best_genotype_combo_prob\":" << bestHetGenotypeComboProb 
                    << ", \"best_genotype_combo_ewens_sampling_probability\":" << bestHetComboAlleleSamplingProb
                    << ", \"combos_tested\":" << bandedCombos.size()
                    << ", \"coverage\":" << alleles.size()
                    << ", \"posterior_normalizer\":" << safe_exp(posteriorNormalizer)
                    << ", \"samples\":";
                json(out, results, parser);
                out << "}" << endl;
            }
            if (pVar >= parameters.PVL) {
                if (parameters.output == "vcf") {
                    string referenceBase = parser->currentReferenceBase();
                    // get the set of unique alternate alleles at this site
                    map<Allele, bool> alternates;
                    for (GenotypeCombo::iterator g = bestHetGenotypeCombo.begin(); g != bestHetGenotypeCombo.end(); ++g) {
                        Genotype* genotype = g->second.first;
                        vector<Allele> alts = genotype->alternateAlleles(referenceBase);
                        for (vector<Allele>::iterator a = alts.begin(); a != alts.end(); ++a)
                            alternates[*a] = true;
                    }
                    // for each unique alternate allele, output a line of vcf
                    for (map<Allele, bool>::iterator alt = alternates.begin(); alt != alternates.end(); ++alt) {
                        out << vcf(pVar,
                                bestHetComboAlleleSamplingProb,
                                alt->first.base(),
                                parser->sampleList,
                                alleles,
                                bestHetGenotypeCombo,
                                results,
                                parser)
                            << endl;
                    }
                }
            }
            
        }
        DEBUG2("finished position");

    }

    trace.close();

    delete parser;

    return 0;

}
