/*****************************************************************************

YASK: Yet Another Stencil Kernel
Copyright (c) 2014-2016, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

*****************************************************************************/

/////////////// Main vector-folding code-generation code. /////////////

// Generation code.
#include "ExprUtils.hpp"
#include "CppIntrin.hpp"
#include "Parse.hpp"

// A register of stencils.
StencilList stencils;
#define REGISTER_STENCIL(Class) static Class registered_ ## Class(stencils)

// Stencils.
#include "ExampleStencil.hpp"
#include "Iso3dfdStencil.hpp"
#include "AveStencil.hpp"
#include "AwpStencil.hpp"

// vars set via cmd-line options.
bool printPseudo = false;
bool printPOVRay = false;
bool printMacros = false;
bool printCpp = false;
bool printKncCpp = false;
bool print512Cpp = false;
bool print256Cpp = false;
int vlenForStats = 0;
StencilBase* stencilFunc = NULL;
string shapeName;
IntTuple foldOptions;                     // vector fold.
IntTuple clusterOptions;                  // cluster sizes.
int exprSize = 50;
bool deferCoeff = false;
int order = 2;
bool firstInner = true;
bool allowUnalignedLoads = false;
string equationTargets;
bool doComb = false;
bool doCse = true;

void usage(const string& cmd) {

    cerr << "Options:\n"
        " -h                print this help message.\n"
        "\n"
        " -st <name>        set stencil type (required); supported stencils:\n";
    for (auto si : stencils) {
        auto name = si.first;
        cerr << "                     " << name << endl;
    }
    cerr <<
        "\n"
        " -fold <dim>=<size>,...    set number of elements in each dimension in a vector block.\n"
        " -cluster <dim>=<size>,... set number of values to evaluate in each dimension.\n"
        " -eq <name>=<substr>,...   put updates to grids containing substring in equation name.\n"
        " -or <order>        set stencil order (ignored for some stencils; default=" << order << ").\n"
        //" -dc                defer coefficient lookup to runtime (for iso3dfd stencil only).\n"
        " -lus               make last dimension of fold unit stride (instead of first).\n"
        " -aul               allow simple unaligned loads (memory map MUST be compatible).\n"
        " -es <expr-size>    set heuristic for expression-size threshold (default=" << exprSize << ").\n"
        " -[no]comb          [do not] combine commutative operations (default=" << doComb << ").\n"
        " -[no]cse           [do not] eliminate common subexpressions (default=" << doCse << ").\n"
        "\n"
        //" -ps <vec-len>      print stats for all folding options for given vector length.\n"
        " -ph                print human-readable scalar pseudo-code for one point.\n"
        " -pp                print POV-Ray code for one fold.\n"
        " -pm                print YASK pre-processor macros.\n"
        " -pcpp              print YASK stencil functions for generic C++.\n"
        " -pknc              print YASK stencil functions for KNC ISA.\n"
        " -p512              print YASK stencil functions for CORE AVX-512 & MIC AVX-512 ISAs.\n"
        " -p256              print YASK stencil functions for CORE AVX & AVX2 ISAs.\n"
        "\n"
        "Examples:\n"
        " " << cmd << " -st iso3dfd -or 8 -fold x=4,y=4 -p256\n"
        " " << cmd << " -st awp -fold y=4,z=2 -p512\n";
    exit(1);
}

// Parse command-line and set global cmd-line option vars.
// Exits on error.
void parseOpts(int argc, const char* argv[]) 
{
    if (argc <= 1)
        usage(argv[0]);

    int argi;               // current arg index.
    for (argi = 1; argi < argc; argi++) {
        if ( argv[argi][0] == '-' && argv[argi][1] ) {
            string opt = argv[argi];

            // options w/o values.
            if (opt == "-h" || opt == "-help" || opt == "--help")
                usage(argv[0]);

            else if (opt == "-ph")
                printPseudo = true;
            else if (opt == "-pp")
                printPOVRay = true;
            else if (opt == "-dc")
                deferCoeff = true;
            else if (opt == "-lus")
                firstInner = false;
            else if (opt == "-aul")
                allowUnalignedLoads = true;
            else if (opt == "-comb")
                doComb = true;
            else if (opt == "-nocomb")
                doComb = false;
            else if (opt == "-cse")
                doCse = true;
            else if (opt == "-nocse")
                doCse = false;
            
            else if (opt == "-pm")
                printMacros = true;
            else if (opt == "-pcpp")
                printCpp = true;
            else if (opt == "-pknc")
                printKncCpp = true;
            else if (opt == "-p512")
                print512Cpp = true;
            else if (opt == "-p256")
                print256Cpp = true;
            
            // add any more options w/o values above.

            // options w/a value.
            else {

                // at least one value needed.
                if (argi + 1 >= argc) {
                    cerr << "error: value missing or bad option '" << opt << "'." << endl;
                    usage(argv[0]);
                }
                string argop = argv[++argi];

                // options w/a string value.
                
                // stencil type.
                if (opt == "-st")
                    shapeName = argop;

                // equations.
                else if (opt == "-eq")
                    equationTargets = argop;

                // fold or cluster
                else if (opt == "-fold" || opt == "-cluster") {

                    // example: x=4,y=2
                    ArgParser ap;
                    ap.parseKeyValuePairs
                        (argop, [&](const string& key, const string& value) {
                            int size = atoi(value.c_str());
                            
                            // set dim in tuple.
                            if (opt == "-fold")
                                foldOptions.addDim(key, size);
                            else
                                clusterOptions.addDim(key, size);
                        });
                }
                
                // add any more options w/a string value above.
                
                else {

                    // options w/an int value.
                    int val = atoi(argop.c_str());

                    if (opt == "-es")
                        exprSize = val;

                    else if (opt == "-or")
                        order = val;

                    else if (opt == "-ps")
                        vlenForStats = val;

                    // add any more options w/int values here.

                    else {
                        cerr << "error: option '" << opt << "' not recognized." << endl;
                        usage(argv[0]);
                    }
                }
            }
        }
        else break;
    }
    if (argi < argc) {
        cerr << "error: unrecognized parameter '" << argv[argi] << "'." << endl;
        usage(argv[0]);
    }
    if (shapeName.length() == 0) {
        cerr << "error: shape not specified." << endl;
        usage(argv[0]);
    }

    // Find the stencil in the registry.
    auto stencilIter = stencils.find(shapeName);
    if (stencilIter == stencils.end()) {
        cerr << "error: unknown stencil shape '" << shapeName << "'." << endl;
        usage(argv[0]);
    }
    stencilFunc = stencilIter->second;
    assert(stencilFunc);
    
    cerr << "Stencil name: " << shapeName << endl;
    if (stencilFunc->usesOrder()) {
        bool orderOk = stencilFunc->setOrder(order);
        if (!orderOk) {
            cerr << "error: invalid order=" << order << " for stencil type '" <<
                shapeName << "'." << endl;
            usage(argv[0]);
        }
        cerr << "Stencil order: " << order << endl;
    }
    cerr << "Expression-size threshold: " << exprSize << endl;
}

// Main program.
int main(int argc, const char* argv[]) {

    // parse options.
    parseOpts(argc, argv);

    // Set default fold ordering.
    IntTuple::setDefaultFirstInner(firstInner);
    
    // Reference to the grids and params in the stencil.
    Grids& grids = stencilFunc->getGrids();
    Params& params = stencilFunc->getParams();

    // Create a union of all dimensions in all grids.
    // Also keep count of how many grids have each dim.
    // Note that dimensions won't be in any particular order!
    IntTuple dimCounts;
    for (auto gp : grids) {

        // Count dimensions from this grid.
        for (auto dim : gp->getDims()) {
            if (dimCounts.lookup(dim))
                dimCounts.setVal(dim, dimCounts.getVal(dim) + 1);
            else
                dimCounts.addDim(dim, 1);
        }
    }

    // For now, there are only global specifications for vector and cluster
    // sizes. Also, vector folding and clustering is done identially for
    // every grid access. Thus, sizes > 1 must exist in all grids.  So, init
    // vector and cluster sizes based on dimensions that appear in ALL
    // grids.
    // TODO: relax this restriction.
    IntTuple foldLengths, clusterLengths;
    for (auto dim : dimCounts.getDims()) {
        if (dimCounts.getVal(dim) == (int)grids.size()) {
            foldLengths.addDim(dim, 1);
            clusterLengths.addDim(dim, 1);
        }
    }

    // Create final fold lengths based on cmd-line options.
    IntTuple foldLengthsGT1;    // fold dimensions > 1.
    for (auto dim : foldOptions.getDims()) {
        int sz = foldOptions.getVal(dim);
        int* p = foldLengths.lookup(dim);
        if (!p) {
            cerr << "Error: fold-length of " << sz << " in '" << dim <<
                "' dimension not allowed because '" <<
                dim << "' doesn't exist in all grids." << endl;
            exit(1);
        }
        *p = sz;
        if (sz > 1)
            foldLengthsGT1.addDim(dim, sz);
            
    }
    cerr << "Vector-fold dimensions: " << foldLengths.makeDimValStr(" * ") << endl;

    // Checks for unaligned loads.
    if (allowUnalignedLoads) {
        if (foldLengthsGT1.size() > 1) {
            cerr << "Error: attempt to allow unaligned loads when there are " <<
                foldLengthsGT1.size() << " dimensions in the vector-fold that are > 1." << endl;
            exit(1);
        }
        else if (foldLengthsGT1.size() > 0)
            cerr << "Notice: memory map MUST be with unit-stride in " <<
                foldLengthsGT1.makeDimStr() << " dimension!" << endl;
    }

    // Create final cluster lengths based on cmd-line options.
    for (auto dim : clusterOptions.getDims()) {
        int sz = clusterOptions.getVal(dim);
        int* p = clusterLengths.lookup(dim);
        if (!p) {
            cerr << "Error: cluster-length of " << sz << " in '" << dim <<
                "' dimension not allowed because '" <<
                dim << "' doesn't exist in all grids." << endl;
            exit(1);
        }
        *p = sz;
    }
    cerr << "Cluster dimensions: " << clusterLengths.makeDimValStr(" * ") << endl;
    
    // Loop through all points in a cluster.
    // For each point, determine the offset from 0,..,0 based
    // on the cluster point and fold lengths.
    // Then, construct an AST for all equations at this offset.
    // When done, for each equation, we will have an AST for each
    // cluster point stored in its respective grid.
    // TODO: check for illegal dependences between cluster points.
    clusterLengths.visitAllPoints([&](const IntTuple& clusterPoint){
            
            // Get starting offset of cluster, which is each cluster index
            // multipled by corresponding vector size.
            auto offsets = clusterPoint.multElements(foldLengths);

            // Add in any dims not in the cluster.
            for (auto dim : dimCounts.getDims()) {
                if (!offsets.lookup(dim))
                    offsets.addDim(dim, 0);
            }
            
            // Construct AST in grids for this cluster point.
            stencilFunc->define(offsets);
        });

    // Extract equations from grids.
    Equations equations;
    equations.findEquations(grids, equationTargets);
    equations.printInfo(cerr);

    // Get stats.
    {
        CounterVisitor cv;
        grids.acceptToFirst(&cv);
        cv.printStats(cerr, "for one vector");
    }
    if (clusterLengths.product() > 1) {
        CounterVisitor cv;
        grids.acceptToAll(&cv);
        cv.printStats(cerr, "for one cluster");
    }
    
    // Make a list of optimizations to apply.
    vector<OptVisitor*> opts;
    if (doCse)
        opts.push_back(new CseVisitor);
    if (doComb) {
        opts.push_back(new CombineVisitor);
        if (doCse)
            opts.push_back(new CseVisitor);
    }
    
    // Apply opts.
    for (auto optimizer : opts) {

        grids.acceptToAll(optimizer);
        int numChanges = optimizer->getNumChanges();
        string descr = "after applying " + optimizer->getName();

        // Get new stats.
        if (numChanges) {
            CounterVisitor cv;
            grids.acceptToAll(&cv);
            cv.printStats(cerr, descr);
            //addComment(cerr, grids);
        }
        else
            cerr << "No changes " << descr << '.' << endl;
    }

    ///// Print out above data based on -p* option(s).
    
    // Human-readable output.
    if (printPseudo) {
        PseudoPrinter printer(*stencilFunc, equations, exprSize);
        printer.print(cout);
    }

    // POV-Ray output.
    if (printPOVRay) {
        POVRayPrinter printer(*stencilFunc, equations, exprSize);
        printer.print(cout);
    }

    // Print YASK classes to update grids and/or prefetch.
    if (printCpp) {
        YASKCppPrinter printer(*stencilFunc, equations, exprSize,
                               allowUnalignedLoads, dimCounts,
                               foldLengths, clusterLengths);
        printer.printCode(cout);
    }
    if (printKncCpp) {
        YASKKncPrinter printer(*stencilFunc, equations, exprSize,
                               allowUnalignedLoads, dimCounts,
                               foldLengths, clusterLengths);
        printer.printCode(cout);
    }
    if (print512Cpp) {
        YASKAvx512Printer printer(*stencilFunc, equations, exprSize,
                                  allowUnalignedLoads, dimCounts,
                                  foldLengths, clusterLengths);
        printer.printCode(cout);
    }
    if (print256Cpp) {
        YASKAvx256Printer printer(*stencilFunc, equations, exprSize,
                                  allowUnalignedLoads, dimCounts,
                                  foldLengths, clusterLengths);
        printer.printCode(cout);
    }

    // Print CPP macros.
    if (printMacros) {
        YASKCppPrinter printer(*stencilFunc, equations, exprSize,
                               allowUnalignedLoads, dimCounts,
                               foldLengths, clusterLengths);
        printer.printMacros(cout);
    }
    
#if 0
    // Print stats for various folding options.
    if (vlenForStats) {
        string separator(",");
        VecInfoVisitor::printStatsHeader(cout, separator);

        // Loop through all grids.
        for (auto gp : grids) {

            // Loop through possible folds of given length.
            for (int xlen = vlenForStats; xlen > 0; xlen--) {
                for (int ylen = vlenForStats / xlen; ylen > 0; ylen--) {
                    int zlen = vlenForStats / xlen / ylen;
                    if (vlenForStats == xlen * ylen * zlen) {
                        
                        // Create vectors needed to implement RHS.
                        VecInfoVisitor vv(xlen, ylen, zlen);
                        gp->acceptToAll(&vv);
                        
                        // Print stats.
                        vv.printStats(cout, gp->getName(), separator);
                    }
                }
            }
        }
    }
#endif
    
    return 0;
}
