#include <petsc/private/pcimpl.h>     /*I "petscpc.h" I*/
#include <petsc.h>
#include <petsc/private/hash.h>
#include <petscsf.h>
#include <libssc.h>

PetscLogEvent PC_Patch_CreatePatches, PC_Patch_ComputeOp, PC_Patch_Solve, PC_Patch_Scatter, PC_Patch_Apply;

static PetscBool PCPatchPackageInitialized = PETSC_FALSE;

#undef __FUNCT__
#define __FUNCT__ "PCPatchInitializePackage"
PETSC_EXTERN PetscErrorCode PCPatchInitializePackage(void)
{
    PetscErrorCode ierr;
    PetscFunctionBegin;

    if (PCPatchPackageInitialized) PetscFunctionReturn(0);
    PCPatchPackageInitialized = PETSC_TRUE;
    ierr = PCRegister("patch", PCCreate_PATCH); CHKERRQ(ierr);
    ierr = PetscLogEventRegister("PCPATCHCreate", PC_CLASSID, &PC_Patch_CreatePatches); CHKERRQ(ierr);
    ierr = PetscLogEventRegister("PCPATCHComputeOp", PC_CLASSID, &PC_Patch_ComputeOp); CHKERRQ(ierr);
    ierr = PetscLogEventRegister("PCPATCHSolve", PC_CLASSID, &PC_Patch_Solve); CHKERRQ(ierr);
    ierr = PetscLogEventRegister("PCPATCHApply", PC_CLASSID, &PC_Patch_Apply); CHKERRQ(ierr);
    ierr = PetscLogEventRegister("PCPATCHScatter", PC_CLASSID, &PC_Patch_Scatter); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

typedef struct {
    DM              dm;         /* DMPlex object describing mesh
                                 * topology (need not be the same as
                                 * PC's DM) */
    PetscSF         defaultSF;
    PetscSection    dofSection;
    PetscSection    cellCounts;
    PetscSection    cellNumbering; /* Numbering of cells in DM */
    PetscSection    gtolCounts;   /* Indices to extract from local to
                                   * patch vectors */
    PetscSection    bcCounts;
    IS              cells;
    IS              dofs;
    IS              bcNodes;
    IS              gtol;
    IS             *bcs;

    MPI_Datatype    data_type;
    PetscBool       free_type;

    PetscBool       save_operators; /* Save all operators (or create/destroy one at a time?) */
    PetscBool       partition_of_unity; /* Weight updates by dof multiplicity? */
    PetscInt        npatch;     /* Number of patches */
    PetscInt        bs;            /* block size (can come from global
                                    * operators?) */
    PetscInt        nodesPerCell;
    const PetscInt *cellNodeMap; /* Map from cells to nodes */

    KSP            *ksp;        /* Solvers for each patch */
    Vec             localX, localY;
    Vec             dof_weights; /* In how many patches does each dof lie? */
    Vec            *patchX, *patchY; /* Work vectors for patches */
    Mat            *mat;        /* Operators */
    MatType         sub_mat_type;
    PetscErrorCode (*usercomputeop)(PC, Mat, PetscInt, const PetscInt *, PetscInt, const PetscInt *, void *);
    void           *usercomputectx;
} PC_PATCH;

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDMPlex"
PETSC_EXTERN PetscErrorCode PCPatchSetDMPlex(PC pc, DM dm)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->dm = dm;
    ierr = PetscObjectReference((PetscObject)dm); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetSaveOperators"
PETSC_EXTERN PetscErrorCode PCPatchSetSaveOperators(PC pc, PetscBool flg)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->save_operators = flg;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetPartitionOfUnity"
PETSC_EXTERN PetscErrorCode PCPatchSetPartitionOfUnity(PC pc, PetscBool flg)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->partition_of_unity = flg;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDefaultSF"
PETSC_EXTERN PetscErrorCode PCPatchSetDefaultSF(PC pc, PetscSF sf)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->defaultSF = sf;
    ierr = PetscObjectReference((PetscObject)sf); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetCellNumbering"
PETSC_EXTERN PetscErrorCode PCPatchSetCellNumbering(PC pc, PetscSection cellNumbering)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->cellNumbering = cellNumbering;
    ierr = PetscObjectReference((PetscObject)cellNumbering); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "PCPatchSetDiscretisationInfo"
PETSC_EXTERN PetscErrorCode PCPatchSetDiscretisationInfo(PC pc, PetscSection dofSection,
                                                         PetscInt bs,
                                                         PetscInt nodesPerCell,
                                                         const PetscInt *cellNodeMap,
                                                         PetscInt numBcs,
                                                         const PetscInt *bcNodes)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;

    patch->dofSection = dofSection;
    ierr = PetscObjectReference((PetscObject)dofSection); CHKERRQ(ierr);
    patch->bs = bs;
    patch->nodesPerCell = nodesPerCell;
    /* Not freed here. */
    patch->cellNodeMap = cellNodeMap;
    ierr = ISCreateGeneral(PETSC_COMM_SELF, numBcs, bcNodes, PETSC_COPY_VALUES, &patch->bcNodes); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetSubMatType"
PETSC_EXTERN PetscErrorCode PCPatchSetSubMatType(PC pc, MatType sub_mat_type)
{
    PetscErrorCode ierr;
    PC_PATCH      *patch = (PC_PATCH *)pc->data;
    PetscFunctionBegin;
    if (patch->sub_mat_type) {
        ierr = PetscFree(patch->sub_mat_type); CHKERRQ(ierr);
    }
    ierr = PetscStrallocpy(sub_mat_type, (char **)&patch->sub_mat_type); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchSetComputeOperator"
PETSC_EXTERN PetscErrorCode PCPatchSetComputeOperator(PC pc, PetscErrorCode (*func)(PC, Mat, PetscInt,
                                                                                    const PetscInt *,
                                                                                    PetscInt,
                                                                                    const PetscInt *,
                                                                                    void *),
                                                      void *ctx)
{
    PC_PATCH *patch = (PC_PATCH *)pc->data;

    PetscFunctionBegin;
    /* User op can assume matrix is zeroed */
    patch->usercomputeop = func;
    patch->usercomputectx = ctx;

    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatches"
/*
 * PCPatchCreateCellPatches - create patches of cells around vertices in the mesh.
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 *
 * Output Parameters:
 * + cellCounts - Section with counts of cells around each vertex
 * - cells - IS of the cell point indices of cells in each patch
 */
static PetscErrorCode PCPatchCreateCellPatches(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch      = (PC_PATCH *)pc->data;
    DM              dm;
    DMLabel         ghost;
    PetscInt        pStart, pEnd, vStart, vEnd, cStart, cEnd;
    PetscBool       flg;
    PetscInt        closureSize;
    PetscInt       *closure    = NULL;
    PetscInt       *cellsArray = NULL;
    PetscInt        numCells;
    PetscSection    cellCounts;

    PetscFunctionBegin;

    dm = patch->dm;
    if (!dm) {
        SETERRQ(PetscObjectComm((PetscObject)pc), PETSC_ERR_ARG_WRONGSTATE, "DM not yet set on patch PC\n");
    }
    ierr = DMPlexGetChart(dm, &pStart, &pEnd); CHKERRQ(ierr);
    ierr = DMPlexGetDepthStratum(dm, 0, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = DMPlexGetHeightStratum(dm, 0, &cStart, &cEnd); CHKERRQ(ierr);

    /* These labels mark the owned points.  We only create patches
     * around vertices that this process owns. */
    ierr = DMGetLabel(dm, "pyop2_ghost", &ghost); CHKERRQ(ierr);

    ierr = DMLabelCreateIndex(ghost, pStart, pEnd); CHKERRQ(ierr);

    ierr = PetscSectionCreate(PETSC_COMM_SELF, &patch->cellCounts); CHKERRQ(ierr);
    cellCounts = patch->cellCounts;
    ierr = PetscSectionSetChart(cellCounts, vStart, vEnd); CHKERRQ(ierr);

    /* Count cells surrounding each vertex */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        ierr = DMLabelHasPoint(ghost, v, &flg); CHKERRQ(ierr);
        /* Not an owned vertex, don't make a cell patch. */
        if (flg) {
            continue;
        }
        ierr = DMPlexGetTransitiveClosure(dm, v, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);
        for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
            const PetscInt c = closure[2*ci];
            if (cStart <= c && c < cEnd) {
                ierr = PetscSectionAddDof(cellCounts, v, 1); CHKERRQ(ierr);
            }
        }
    }
    ierr = DMLabelDestroyIndex(ghost); CHKERRQ(ierr);

    ierr = PetscSectionSetUp(cellCounts); CHKERRQ(ierr);
    ierr = PetscSectionGetStorageSize(cellCounts, &numCells); CHKERRQ(ierr);
    ierr = PetscMalloc1(numCells, &cellsArray); CHKERRQ(ierr);

    /* Now that we know how much space we need, run through again and
     * actually remember the cells. */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt ndof, off;
        ierr = PetscSectionGetDof(cellCounts, v, &ndof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        if ( ndof <= 0 ) {
            continue;
        }
        ierr = DMPlexGetTransitiveClosure(dm, v, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);
        ndof = 0;
        for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
            const PetscInt c = closure[2*ci];
            if (cStart <= c && c < cEnd) {
                cellsArray[ndof + off] = c;
                ndof++;
            }
        }
    }
    if (closure) {
        ierr = DMPlexRestoreTransitiveClosure(dm, 0, PETSC_FALSE, &closureSize, &closure); CHKERRQ(ierr);
    }

    ierr = ISCreateGeneral(PETSC_COMM_SELF, numCells, cellsArray, PETSC_OWN_POINTER, &patch->cells); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(patch->cellCounts, &pStart, &pEnd); CHKERRQ(ierr);
    patch->npatch = pEnd - pStart;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchFacets"
/*
 * PCPatchCreateCellPatchFacets - Build the boundary facets for each cell patch
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 * . cellCounts - Section with counts of cells around each vertex
 * - cells - IS of the cell point indices of cells in each patch
 *
 * Output Parameters:
 * + facetCounts - Section with counts of boundary facets for each cell patch
 * - facets - IS of the boundary facet point indices for each cell patch.
 *
 * Note:
 *  The output facets do not include those facets that are the
 *  boundary of the domain, they are treated separately.
 */
static PetscErrorCode PCPatchCreateCellPatchFacets(PC pc, PetscSection *facetCounts, IS *facets)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch       = (PC_PATCH *)pc->data;
    DM              dm          = patch->dm;
    PetscSection    cellCounts  = patch->cellCounts;
    IS              cells       = patch->cells;
    PetscInt        vStart, vEnd, fStart, fEnd;
    DMLabel         facetLabel;
    PetscBool       flg;
    PetscInt        totalFacets, facetIndex;
    const PetscInt *cellFacets  = NULL;
    const PetscInt *facetCells  = NULL;
    PetscInt       *facetsArray = NULL;
    const PetscInt *cellsArray  = NULL;
    PetscHashI      ht;

    PetscFunctionBegin;

    /* This label marks facets exterior to the domain, which we don't
     * treat here. */
    ierr = DMGetLabel(dm, "exterior_facets", &facetLabel); CHKERRQ(ierr);

    ierr = DMPlexGetDepthStratum(dm, 0, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = DMPlexGetHeightStratum(dm, 1, &fStart, &fEnd); CHKERRQ(ierr);
    ierr = DMLabelCreateIndex(facetLabel, fStart, fEnd); CHKERRQ(ierr);

    ierr = PetscSectionCreate(PETSC_COMM_SELF, facetCounts); CHKERRQ(ierr);
    ierr = PetscSectionSetChart(*facetCounts, vStart, vEnd); CHKERRQ(ierr);

    /* OK, so now we know the cells in each patch, and need to
     * determine the facets that live on the boundary of each patch.
     * We will apply homogeneous dirichlet bcs to the dofs on the
     * boundary.  The exception is for facets that are exterior to
     * the whole domain (where the normal bcs are applied). */

    /* Used to keep track of the cells in the patch. */
    PetscHashICreate(ht);

    /* Guess at number of facets: each cell contributes one facet to
     * the boundary facets.  Hopefully we will only realloc a little
     * bit.  This is a good guess for simplices, but not as good for
     * quads. */
    ierr = ISGetSize(cells, &totalFacets); CHKERRQ(ierr);
    ierr = PetscMalloc1(totalFacets, &facetsArray); CHKERRQ(ierr);
    ierr = ISGetIndices(cells, &cellsArray); CHKERRQ(ierr);
    facetIndex = 0;
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt ndof, off;
        ierr = PetscSectionGetDof(cellCounts, v, &ndof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        if ( ndof <= 0 ) {
            /* No cells around this vertex. */
            continue;
        }
        PetscHashIClear(ht);
        for ( PetscInt ci = off; ci < ndof + off; ci++ ) {
            const PetscInt c = cellsArray[ci];
            PetscHashIAdd(ht, c, 0);
        }
        for ( PetscInt ci = off; ci < ndof + off; ci++ ) {
            const PetscInt c = cellsArray[ci];
            PetscInt       numFacets, numCells;
            /* Facets of each cell */
            ierr = DMPlexGetCone(dm, c, &cellFacets); CHKERRQ(ierr);
            ierr = DMPlexGetConeSize(dm, c, &numFacets); CHKERRQ(ierr);
            for ( PetscInt j = 0; j < numFacets; j++ ) {
                const PetscInt f = cellFacets[j];
                ierr = DMLabelHasPoint(facetLabel, f, &flg); CHKERRQ(ierr);
                if (flg) {
                    /* Facet is on domain boundary, don't select it */
                    continue;
                }
                /* Cells in the support of the facet */
                ierr = DMPlexGetSupport(dm, f, &facetCells); CHKERRQ(ierr);
                ierr = DMPlexGetSupportSize(dm, f, &numCells); CHKERRQ(ierr);
                if (numCells == 1) {
                    /* This facet is on a process boundary, therefore
                     * also a patch boundary */
                    ierr = PetscSectionAddDof(*facetCounts, v, 1); CHKERRQ(ierr);
                    goto addFacet;
                } else {
                    for ( PetscInt k = 0; k < numCells; k++ ) {
                        PetscHashIHasKey(ht, facetCells[k], flg);
                        if (!flg) {
                            /* Facet's cell is not in the patch, so
                             * it's on the patch boundary. */
                            ierr = PetscSectionAddDof(*facetCounts, v, 1); CHKERRQ(ierr);
                            goto addFacet;
                        }
                    }
                }
                continue;
            addFacet:
                if (facetIndex >= totalFacets) {
                    totalFacets = (PetscInt)((1 + totalFacets)*1.2);
                    ierr = PetscRealloc(sizeof(PetscInt)*totalFacets, &facetsArray); CHKERRQ(ierr);
                }
                facetsArray[facetIndex++] = f;
            }
        }
    }
    ierr = DMLabelDestroyIndex(facetLabel); CHKERRQ(ierr);
    ierr = ISRestoreIndices(cells, &cellsArray); CHKERRQ(ierr);
    PetscHashIDestroy(ht);

    ierr = PetscSectionSetUp(*facetCounts); CHKERRQ(ierr);
    ierr = PetscRealloc(sizeof(PetscInt)*facetIndex, &facetsArray); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, facetIndex, facetsArray, PETSC_OWN_POINTER, facets); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchDiscretisationInfo"
/*
 * PCPatchCreateCellPatchDiscretisationInfo - Build the dof maps for cell patches
 *
 * Input Parameters:
 * + dm - The DMPlex object defining the mesh
 * . cellCounts - Section with counts of cells around each vertex
 * . cells - IS of the cell point indices of cells in each patch
 * . facetCounts - Section with counts of boundary facets for each cell patch
 * . facets - IS of the boundary facet point indices for each cell patch.
 * . cellNumbering - Section mapping plex cell points to Firedrake cell indices.
 * . dofsPerCell - number of dofs per cell.
 * - cellNodeMap - map from cells to dof indices (dofsPerCell * numCells)
 *
 * Output Parameters:
 * + dofs - IS of local dof numbers of each cell in the patch
 * . gtolCounts - Section with counts of dofs per cell patch
 * - gtol - IS mapping from global dofs to local dofs for each patch. 
 */
static PetscErrorCode PCPatchCreateCellPatchDiscretisationInfo(PC pc,
                                                               PetscSection facetCounts,
                                                               IS facets)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch           = (PC_PATCH *)pc->data;
    PetscSection    cellCounts      = patch->cellCounts;
    PetscSection    gtolCounts;
    IS              cells           = patch->cells;
    PetscSection    cellNumbering   = patch->cellNumbering;
    const PetscInt  dofsPerCell     = patch->nodesPerCell;
    const PetscInt *cellNodeMap     = patch->cellNodeMap;
    PetscInt        numCells;
    PetscInt        numDofs;
    PetscInt        numGlobalDofs;
    PetscInt        vStart, vEnd;
    const PetscInt *cellsArray;
    PetscInt       *newCellsArray   = NULL;
    PetscInt       *dofsArray       = NULL;
    PetscInt       *globalDofsArray = NULL;
    PetscInt        globalIndex     = 0;
    PetscHashI      ht;
    PetscFunctionBegin;

    /* dofcounts section is cellcounts section * dofPerCell */
    ierr = PetscSectionGetStorageSize(cellCounts, &numCells); CHKERRQ(ierr);
    numDofs = numCells * dofsPerCell;
    ierr = PetscMalloc1(numDofs, &dofsArray); CHKERRQ(ierr);
    ierr = PetscMalloc1(numCells, &newCellsArray); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(cellCounts, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = PetscSectionCreate(PETSC_COMM_SELF, &patch->gtolCounts); CHKERRQ(ierr);
    gtolCounts = patch->gtolCounts;
    ierr = PetscSectionSetChart(gtolCounts, vStart, vEnd); CHKERRQ(ierr);

    ierr = ISGetIndices(cells, &cellsArray);
    PetscHashICreate(ht);
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt dof, off;
        PetscInt localIndex = 0;
        PetscHashIClear(ht);
        ierr = PetscSectionGetDof(cellCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            /* Walk over the cells in this patch. */
            const PetscInt c = cellsArray[i];
            PetscInt cell;
            ierr = PetscSectionGetDof(cellNumbering, c, &cell); CHKERRQ(ierr);
            if ( cell <= 0 ) {
                SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_ARG_OUTOFRANGE,
                        "Cell doesn't appear in cell numbering map");
            }
            ierr = PetscSectionGetOffset(cellNumbering, c, &cell); CHKERRQ(ierr);
            newCellsArray[i] = cell;
            for ( PetscInt j = 0; j < dofsPerCell; j++ ) {
                /* For each global dof, map it into contiguous local storage. */
                const PetscInt globalDof = cellNodeMap[cell*dofsPerCell + j];
                PetscInt localDof;
                PetscHashIMap(ht, globalDof, localDof);
                if (localDof == -1) {
                    localDof = localIndex++;
                    PetscHashIAdd(ht, globalDof, localDof);
                }
                if ( globalIndex >= numDofs ) {
                    SETERRQ(PETSC_COMM_WORLD, PETSC_ERR_ARG_OUTOFRANGE,
                            "Found more dofs than expected");
                }
                /* And store. */
                dofsArray[globalIndex++] = localDof;
            }
        }
        PetscHashISize(ht, dof);
        /* How many local dofs in this patch? */
        ierr = PetscSectionSetDof(gtolCounts, v, dof); CHKERRQ(ierr);
    }
    ierr = PetscSectionSetUp(gtolCounts); CHKERRQ(ierr);
    ierr = PetscSectionGetStorageSize(gtolCounts, &numGlobalDofs); CHKERRQ(ierr);
    ierr = PetscMalloc1(numGlobalDofs, &globalDofsArray); CHKERRQ(ierr);

    /* Now populate the global to local map.  This could be merged
    * into the above loop if we were willing to deal with reallocs. */
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt       dof, off;
        PetscHashIIter hi;
        PetscHashIClear(ht);
        ierr = PetscSectionGetDof(cellCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(cellCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            /* Reconstruct mapping of global-to-local on this patch. */
            const PetscInt c = cellsArray[i];
            PetscInt cell;
            ierr = PetscSectionGetOffset(cellNumbering, c, &cell); CHKERRQ(ierr);
            for ( PetscInt j = 0; j < dofsPerCell; j++ ) {
                const PetscInt globalDof = cellNodeMap[cell*dofsPerCell + j];
                const PetscInt localDof = dofsArray[i*dofsPerCell + j];
                PetscHashIAdd(ht, globalDof, localDof);
            }
        }
        if (dof > 0) {
            /* Shove it in the output data structure. */
            ierr = PetscSectionGetOffset(gtolCounts, v, &off); CHKERRQ(ierr);
            PetscHashIIterBegin(ht, hi);
            while (!PetscHashIIterAtEnd(ht, hi)) {
                PetscInt globalDof, localDof;
                PetscHashIIterGetKeyVal(ht, hi, globalDof, localDof);
                if (globalDof >= 0) {
                    globalDofsArray[off + localDof] = globalDof;
                }
                PetscHashIIterNext(ht, hi);
            }
        }
    }
    PetscHashIDestroy(ht);
    ierr = ISRestoreIndices(cells, &cellsArray);

    /* Replace cell indices with firedrake-numbered ones. */
    ierr = ISGeneralSetIndices(cells, numCells, (const PetscInt *)newCellsArray, PETSC_OWN_POINTER); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, numGlobalDofs, globalDofsArray, PETSC_OWN_POINTER, &patch->gtol); CHKERRQ(ierr);
    ierr = ISCreateGeneral(PETSC_COMM_SELF, numDofs, dofsArray, PETSC_OWN_POINTER, &patch->dofs); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateCellPatchBCs"
static PetscErrorCode PCPatchCreateCellPatchBCs(PC pc,
                                                PetscSection facetCounts,
                                                IS facets)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch      = (PC_PATCH *)pc->data;
    DM              dm         = patch->dm;
    PetscInt        numBcs;
    const PetscInt *bcNodes    = NULL;
    PetscSection    dofSection = patch->dofSection;
    PetscSection    gtolCounts = patch->gtolCounts;
    PetscSection    bcCounts;
    IS              gtol = patch->gtol;
    PetscHashI      globalBcs;
    PetscHashI      localBcs;
    PetscHashI      patchDofs;
    PetscInt       *bcsArray   = NULL;
    PetscInt        vStart, vEnd;
    PetscInt        closureSize;
    PetscInt       *closure    = NULL;
    const PetscInt *gtolArray;
    const PetscInt *facetsArray;
    PetscFunctionBegin;

    PetscHashICreate(globalBcs);
    ierr = ISGetIndices(patch->bcNodes, &bcNodes); CHKERRQ(ierr);
    ierr = ISGetSize(patch->bcNodes, &numBcs); CHKERRQ(ierr);
    for ( PetscInt i = 0; i < numBcs; i++ ) {
        PetscHashIAdd(globalBcs, bcNodes[i], 0);
    }
    ierr = ISRestoreIndices(patch->bcNodes, &bcNodes); CHKERRQ(ierr);
    PetscHashICreate(patchDofs);
    PetscHashICreate(localBcs);

    ierr = PetscSectionGetChart(facetCounts, &vStart, &vEnd); CHKERRQ(ierr);
    ierr = PetscSectionCreate(PETSC_COMM_SELF, &patch->bcCounts); CHKERRQ(ierr);
    bcCounts = patch->bcCounts;
    ierr = PetscSectionSetChart(bcCounts, vStart, vEnd); CHKERRQ(ierr);
    ierr = PetscMalloc1(vEnd - vStart, &patch->bcs); CHKERRQ(ierr);

    ierr = ISGetIndices(gtol, &gtolArray); CHKERRQ(ierr);
    ierr = ISGetIndices(facets, &facetsArray); CHKERRQ(ierr);
    for ( PetscInt v = vStart; v < vEnd; v++ ) {
        PetscInt numBcs, dof, off;
        PetscInt bcIndex = 0;
        PetscHashIClear(patchDofs);
        PetscHashIClear(localBcs);
        ierr = PetscSectionGetDof(gtolCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(gtolCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            PetscBool flg;
            const PetscInt globalDof = gtolArray[i];
            const PetscInt localDof = i - off;
            PetscHashIAdd(patchDofs, globalDof, localDof);
            PetscHashIHasKey(globalBcs, globalDof, flg);
            if (flg) {
                PetscHashIAdd(localBcs, localDof, 0);
            }
        }
        ierr = PetscSectionGetDof(facetCounts, v, &dof); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(facetCounts, v, &off); CHKERRQ(ierr);
        for ( PetscInt i = off; i < off + dof; i++ ) {
            const PetscInt f = facetsArray[i];
            ierr = DMPlexGetTransitiveClosure(dm, f, PETSC_TRUE, &closureSize, &closure); CHKERRQ(ierr);
            for ( PetscInt ci = 0; ci < closureSize; ci++ ) {
                PetscInt ldof, loff;
                const PetscInt p = closure[2*ci];
                ierr = PetscSectionGetDof(dofSection, p, &ldof); CHKERRQ(ierr);
                ierr = PetscSectionGetOffset(dofSection, p, &loff); CHKERRQ(ierr);
                if ( ldof > 0 ) {
                    for ( PetscInt j = loff; j < ldof + loff; j++ ) {
                        PetscInt localDof;
                        PetscHashIMap(patchDofs, j, localDof);
                        if ( localDof == -1 ) {
                            SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_OUTOFRANGE,
                                    "Didn't find facet dof in patch dof\n");
                        }
                        PetscHashIAdd(localBcs, localDof, 0);
                    }
                }
            }
        }
        /* OK, now we have a hash table with all the bcs indicated by
         * the facets and global bcs */
        PetscHashISize(localBcs, numBcs);
        ierr = PetscSectionSetDof(bcCounts, v, numBcs); CHKERRQ(ierr);
        ierr = PetscMalloc1(numBcs, &bcsArray); CHKERRQ(ierr);
        ierr = PetscHashIGetKeys(localBcs, &bcIndex, bcsArray); CHKERRQ(ierr);
        ierr = PetscSortInt(numBcs, bcsArray); CHKERRQ(ierr);
        ierr = ISCreateBlock(PETSC_COMM_SELF, patch->bs, numBcs, bcsArray, PETSC_OWN_POINTER, &(patch->bcs[v - vStart])); CHKERRQ(ierr);
    }
    if (closure) {
        ierr = DMPlexRestoreTransitiveClosure(dm, 0, PETSC_TRUE, &closureSize, &closure); CHKERRQ(ierr);
    }
    ierr = ISRestoreIndices(gtol, &gtolArray); CHKERRQ(ierr);
    ierr = ISRestoreIndices(facets, &facetsArray); CHKERRQ(ierr);
    PetscHashIDestroy(localBcs);
    PetscHashIDestroy(patchDofs);
    PetscHashIDestroy(globalBcs);

    ierr = PetscSectionSetUp(bcCounts); CHKERRQ(ierr);

    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCReset_PATCH"
static PetscErrorCode PCReset_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        i;

    PetscFunctionBegin;
    ierr = DMDestroy(&patch->dm); CHKERRQ(ierr);
    ierr = PetscSFDestroy(&patch->defaultSF); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->dofSection); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->cellCounts); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->cellNumbering); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->gtolCounts); CHKERRQ(ierr);
    ierr = PetscSectionDestroy(&patch->bcCounts); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->gtol); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->cells); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->dofs); CHKERRQ(ierr);
    ierr = ISDestroy(&patch->bcNodes); CHKERRQ(ierr);
    if (patch->bcs) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = ISDestroy(&patch->bcs[i]); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->bcs); CHKERRQ(ierr);
    }

    if (patch->free_type) {
        ierr = MPI_Type_free(&patch->data_type); CHKERRQ(ierr);
        patch->data_type = MPI_DATATYPE_NULL; 
    }

    if (patch->ksp) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = KSPReset(patch->ksp[i]); CHKERRQ(ierr);
        }
    }

    ierr = VecDestroy(&patch->localX); CHKERRQ(ierr);
    ierr = VecDestroy(&patch->localY); CHKERRQ(ierr);
    if (patch->patchX) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = VecDestroy(patch->patchX + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->patchX); CHKERRQ(ierr);
    }
    if (patch->patchY) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = VecDestroy(patch->patchY + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->patchY); CHKERRQ(ierr);
    }
    if (patch->mat) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = MatDestroy(patch->mat + i); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->mat); CHKERRQ(ierr);
    }
    ierr = PetscFree(patch->sub_mat_type); CHKERRQ(ierr);

    patch->free_type = PETSC_FALSE;
    patch->bs = 0;
    patch->cellNodeMap = NULL;
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCDestroy_PATCH"
static PetscErrorCode PCDestroy_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        i;

    PetscFunctionBegin;

    ierr = PCReset_PATCH(pc); CHKERRQ(ierr);
    if (patch->ksp) {
        for ( i = 0; i < patch->npatch; i++ ) {
            ierr = KSPDestroy(&patch->ksp[i]); CHKERRQ(ierr);
        }
        ierr = PetscFree(patch->ksp); CHKERRQ(ierr);
    }
    ierr = PetscFree(pc->data); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchCreateMatrix"
static PetscErrorCode PCPatchCreateMatrix(PC pc, Vec x, Vec y, Mat *mat)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscInt        csize, rsize, cbs, rbs;

    PetscFunctionBegin;
    ierr = VecGetSize(x, &csize); CHKERRQ(ierr);
    ierr = VecGetBlockSize(x, &cbs); CHKERRQ(ierr);
    ierr = VecGetSize(y, &rsize); CHKERRQ(ierr);
    ierr = VecGetBlockSize(y, &rbs); CHKERRQ(ierr);
    ierr = MatCreate(PETSC_COMM_SELF, mat); CHKERRQ(ierr);
    if (patch->sub_mat_type) {
        ierr = MatSetType(*mat, patch->sub_mat_type); CHKERRQ(ierr);
    }
    ierr = MatSetSizes(*mat, rsize, csize, rsize, csize); CHKERRQ(ierr);
    ierr = MatSetBlockSizes(*mat, rbs, cbs); CHKERRQ(ierr);
    ierr = MatSetUp(*mat); CHKERRQ(ierr);
    
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatchComputeOperator"
static PetscErrorCode PCPatchComputeOperator(PC pc, Mat mat, PetscInt which)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    const PetscInt *dofsArray;
    const PetscInt *cellsArray;
    PetscInt        ncell, offset, pStart, pEnd;

    PetscFunctionBegin;

    ierr = PetscLogEventBegin(PC_Patch_ComputeOp, pc, 0, 0, 0); CHKERRQ(ierr);
    if (!patch->usercomputeop) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONGSTATE, "Must call PCPatchSetComputeOperator() to set user callback\n");
    }
    ierr = ISGetIndices(patch->dofs, &dofsArray); CHKERRQ(ierr);
    ierr = ISGetIndices(patch->cells, &cellsArray); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(patch->cellCounts, &pStart, &pEnd); CHKERRQ(ierr);

    which += pStart;
    if (which >= pEnd) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_OUTOFRANGE, "Asked for operator index is invalid\n"); CHKERRQ(ierr);
    }

    ierr = PetscSectionGetDof(patch->cellCounts, which, &ncell); CHKERRQ(ierr);
    ierr = PetscSectionGetOffset(patch->cellCounts, which, &offset); CHKERRQ(ierr);
    PetscStackPush("PCPatch user callback");
    ierr = patch->usercomputeop(pc, mat, ncell, cellsArray + offset, ncell*patch->nodesPerCell, dofsArray + offset*patch->nodesPerCell, patch->usercomputectx); CHKERRQ(ierr);
    PetscStackPop;
    ierr = ISRestoreIndices(patch->dofs, &dofsArray); CHKERRQ(ierr);
    ierr = ISRestoreIndices(patch->cells, &cellsArray); CHKERRQ(ierr);
    /* Apply boundary conditions.  Could also do this through the local_to_patch guy. */
    ierr = MatZeroRowsColumnsIS(mat, patch->bcs[which-pStart], (PetscScalar)1.0, NULL, NULL); CHKERRQ(ierr);
    ierr = PetscLogEventEnd(PC_Patch_ComputeOp, pc, 0, 0, 0); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCPatch_ScatterLocal_Private"
static PetscErrorCode PCPatch_ScatterLocal_Private(PC pc, PetscInt p,
                                                   Vec x, Vec y,
                                                   InsertMode mode,
                                                   ScatterMode scat)
{
    PetscErrorCode ierr;
    PC_PATCH          *patch   = (PC_PATCH *)pc->data;
    const PetscScalar *xArray = NULL;
    PetscScalar *yArray = NULL;
    const PetscInt *gtolArray = NULL;
    PetscInt offset, size;

    PetscFunctionBeginHot;
    ierr = PetscLogEventBegin(PC_Patch_Scatter, pc, 0, 0, 0); CHKERRQ(ierr);

    ierr = VecGetArrayRead(x, &xArray); CHKERRQ(ierr);
    ierr = VecGetArray(y, &yArray); CHKERRQ(ierr);

    ierr = PetscSectionGetDof(patch->gtolCounts, p, &size); CHKERRQ(ierr);
    ierr = PetscSectionGetOffset(patch->gtolCounts, p, &offset); CHKERRQ(ierr);
    ierr = ISGetIndices(patch->gtol, &gtolArray); CHKERRQ(ierr);
    if (mode == INSERT_VALUES && scat != SCATTER_FORWARD) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_INCOMP, "Can't insert if not scattering forward\n");
    }
    if (mode == ADD_VALUES && scat != SCATTER_REVERSE) {
        SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_INCOMP, "Can't add if not scattering reverse\n");
    }
    for ( PetscInt i = 0; i < size; i++ ) {
        for ( PetscInt j = 0; j < patch->bs; j++ ) {
            const PetscInt gidx = gtolArray[i + offset]*patch->bs + j;
            const PetscInt lidx = i*patch->bs + j;
            if (mode == INSERT_VALUES) {
                yArray[lidx] = xArray[gidx];
            } else {
                yArray[gidx] += xArray[lidx];
            }
        }
    }
    ierr = VecRestoreArrayRead(x, &xArray); CHKERRQ(ierr);
    ierr = VecRestoreArray(y, &yArray); CHKERRQ(ierr);
    ierr = ISRestoreIndices(patch->gtol, &gtolArray); CHKERRQ(ierr);
    ierr = PetscLogEventEnd(PC_Patch_Scatter, pc, 0, 0, 0); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "PCSetUp_PATCH"
static PetscErrorCode PCSetUp_PATCH(PC pc)
{
    PetscErrorCode  ierr;
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    const char     *prefix;
    PetscScalar    *patchX  = NULL;
    PetscInt       pStart, numBcs;
    const PetscInt *bcNodes = NULL;

    PetscFunctionBegin;

    if (!pc->setupcalled) {
        PetscSection facetCounts;
        IS           facets;
        PetscInt     pStart, pEnd;
        PetscInt     localSize;
        ierr = PetscLogEventBegin(PC_Patch_CreatePatches, pc, 0, 0, 0); CHKERRQ(ierr);
        switch (patch->bs) {
        case 1:
            patch->data_type = MPIU_SCALAR;
            break;
        case 2:
            patch->data_type = MPIU_2SCALAR;
            break;
        default:
            ierr = MPI_Type_contiguous(patch->bs, MPIU_SCALAR, &patch->data_type); CHKERRQ(ierr);
            ierr = MPI_Type_commit(&patch->data_type); CHKERRQ(ierr);
            patch->free_type = PETSC_TRUE;
        }
        ierr = PetscSectionGetStorageSize(patch->dofSection, &localSize); CHKERRQ(ierr);
        ierr = VecCreateSeq(PETSC_COMM_SELF, localSize*patch->bs, &patch->localX); CHKERRQ(ierr);
        ierr = VecSetBlockSize(patch->localX, patch->bs); CHKERRQ(ierr);
        ierr = VecSetUp(patch->localX); CHKERRQ(ierr);
        ierr = VecDuplicate(patch->localX, &patch->localY); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatches(pc); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatchFacets(pc, &facetCounts, &facets); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatchDiscretisationInfo(pc, facetCounts, facets); CHKERRQ(ierr);
        ierr = PCPatchCreateCellPatchBCs(pc, facetCounts, facets); CHKERRQ(ierr);
        ierr = PetscSectionDestroy(&facetCounts); CHKERRQ(ierr);
        ierr = ISDestroy(&facets); CHKERRQ(ierr);

        /* OK, now build the work vectors */
        ierr = PetscSectionGetChart(patch->gtolCounts, &pStart, &pEnd); CHKERRQ(ierr);
        ierr = PetscMalloc1(patch->npatch, &patch->patchX); CHKERRQ(ierr);
        ierr = PetscMalloc1(patch->npatch, &patch->patchY); CHKERRQ(ierr);
        for ( PetscInt i = pStart; i < pEnd; i++ ) {
            PetscInt dof;
            ierr = PetscSectionGetDof(patch->gtolCounts, i, &dof); CHKERRQ(ierr);
            ierr = VecCreateSeq(PETSC_COMM_SELF, dof*patch->bs, &patch->patchX[i - pStart]); CHKERRQ(ierr);
            ierr = VecSetBlockSize(patch->patchX[i - pStart], patch->bs); CHKERRQ(ierr);
            ierr = VecSetUp(patch->patchX[i - pStart]); CHKERRQ(ierr);
            ierr = VecCreateSeq(PETSC_COMM_SELF, dof*patch->bs, &patch->patchY[i - pStart]); CHKERRQ(ierr);
            ierr = VecSetBlockSize(patch->patchY[i - pStart], patch->bs); CHKERRQ(ierr);
            ierr = VecSetUp(patch->patchY[i - pStart]); CHKERRQ(ierr);
        }
        ierr = PetscMalloc1(patch->npatch, &patch->ksp); CHKERRQ(ierr);
        ierr = PCGetOptionsPrefix(pc, &prefix); CHKERRQ(ierr);
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = KSPCreate(PETSC_COMM_SELF, patch->ksp + i); CHKERRQ(ierr);
            ierr = KSPSetOptionsPrefix(patch->ksp[i], prefix); CHKERRQ(ierr);
            ierr = KSPAppendOptionsPrefix(patch->ksp[i], "sub_"); CHKERRQ(ierr);
        }
        if (patch->save_operators) {
            ierr = PetscMalloc1(patch->npatch, &patch->mat); CHKERRQ(ierr);
            for ( PetscInt i = 0; i < patch->npatch; i++ ) {
                ierr = PCPatchCreateMatrix(pc, patch->patchX[i], patch->patchY[i], patch->mat + i); CHKERRQ(ierr);
            }
        }
        ierr = PetscLogEventEnd(PC_Patch_CreatePatches, pc, 0, 0, 0); CHKERRQ(ierr);
    }

    /* If desired, calculate weights for dof multiplicity */

    if (patch->partition_of_unity) {
        ierr = VecDuplicate(patch->localX, &patch->dof_weights); CHKERRQ(ierr);
        ierr = PetscSectionGetChart(patch->gtolCounts, &pStart, NULL); CHKERRQ(ierr);
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = VecSet(patch->patchX[i], 1.0); CHKERRQ(ierr);
            /* TODO: Do we need different scatters for X and Y? */
            ierr = VecGetArray(patch->patchX[i], &patchX); CHKERRQ(ierr);
            /* Apply bcs to patchX (zero entries) */
            ierr = ISBlockGetLocalSize(patch->bcs[i], &numBcs); CHKERRQ(ierr);
            ierr = ISBlockGetIndices(patch->bcs[i], &bcNodes); CHKERRQ(ierr);
            for ( PetscInt j = 0; j < numBcs; j++ ) {
                for ( PetscInt k = 0; k < patch->bs; k++ ) {
                    const PetscInt idx = bcNodes[j]*patch->bs + k;
                    patchX[idx] = 0;
                }
            }
            ierr = ISBlockRestoreIndices(patch->bcs[i], &bcNodes); CHKERRQ(ierr);
            ierr = VecRestoreArray(patch->patchX[i], &patchX); CHKERRQ(ierr);

            ierr = PCPatch_ScatterLocal_Private(pc, i + pStart,
                                                patch->patchX[i], patch->dof_weights,
                                                ADD_VALUES, SCATTER_REVERSE); CHKERRQ(ierr);
        }
        ierr = VecReciprocal(patch->dof_weights); CHKERRQ(ierr);
    }

    if (patch->save_operators) {
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = MatZeroEntries(patch->mat[i]); CHKERRQ(ierr);
            ierr = PCPatchComputeOperator(pc, patch->mat[i], i); CHKERRQ(ierr);
            ierr = KSPSetOperators(patch->ksp[i], patch->mat[i], patch->mat[i]); CHKERRQ(ierr);
        }
    }
    if (!pc->setupcalled) {
        for ( PetscInt i = 0; i < patch->npatch; i++ ) {
            ierr = KSPSetFromOptions(patch->ksp[i]); CHKERRQ(ierr);
        }
    }
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCApply_PATCH"
static PetscErrorCode PCApply_PATCH(PC pc, Vec x, Vec y)
{
    PetscErrorCode     ierr;
    PC_PATCH          *patch   = (PC_PATCH *)pc->data;
    const PetscScalar *globalX = NULL;
    PetscScalar       *localX  = NULL;
    PetscScalar       *localY  = NULL;
    PetscScalar       *globalY = NULL;
    PetscScalar       *patchX  = NULL;
    const PetscInt    *bcNodes = NULL;
    PetscInt           pStart, numBcs, size;
    
    PetscFunctionBegin;

    ierr = PetscLogEventBegin(PC_Patch_Apply, pc, 0, 0, 0); CHKERRQ(ierr);
    ierr = PetscOptionsPushGetViewerOff(PETSC_TRUE); CHKERRQ(ierr);
    ierr = VecGetArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = VecGetArray(patch->localX, &localX); CHKERRQ(ierr);
    /* Scatter from global space into overlapped local spaces */
    ierr = PetscSFBcastBegin(patch->defaultSF, patch->data_type, globalX, localX); CHKERRQ(ierr);
    ierr = PetscSFBcastEnd(patch->defaultSF, patch->data_type, globalX, localX); CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = VecRestoreArray(patch->localX, &localX); CHKERRQ(ierr);
    ierr = VecSet(patch->localY, 0.0); CHKERRQ(ierr);
    ierr = PetscSectionGetChart(patch->gtolCounts, &pStart, NULL); CHKERRQ(ierr);
    for ( PetscInt i = 0; i < patch->npatch; i++ ) {
        PetscInt start, len;
        ierr = PetscSectionGetDof(patch->gtolCounts, i + pStart, &len); CHKERRQ(ierr);
        ierr = PetscSectionGetOffset(patch->gtolCounts, i + pStart, &start); CHKERRQ(ierr);
        if ( len <= 0 ) {
            /* TODO: Squash out these guys in the setup as well. */
            continue;
        }
        ierr = PCPatch_ScatterLocal_Private(pc, i + pStart,
                                            patch->localX, patch->patchX[i],
                                            INSERT_VALUES,
                                            SCATTER_FORWARD); CHKERRQ(ierr);
        /* TODO: Do we need different scatters for X and Y? */
        ierr = VecGetArray(patch->patchX[i], &patchX); CHKERRQ(ierr);
        /* Apply bcs to patchX (zero entries) */
        ierr = ISBlockGetLocalSize(patch->bcs[i], &numBcs); CHKERRQ(ierr);
        ierr = ISBlockGetIndices(patch->bcs[i], &bcNodes); CHKERRQ(ierr);
        for ( PetscInt j = 0; j < numBcs; j++ ) {
            for ( PetscInt k = 0; k < patch->bs; k++ ) {
                const PetscInt idx = bcNodes[j]*patch->bs + k;
                patchX[idx] = 0;
            }
        }
        ierr = ISBlockRestoreIndices(patch->bcs[i], &bcNodes); CHKERRQ(ierr);
        ierr = VecRestoreArray(patch->patchX[i], &patchX); CHKERRQ(ierr);
        if (!patch->save_operators) {
            Mat mat;
            ierr = PCPatchCreateMatrix(pc, patch->patchX[i], patch->patchY[i], &mat); CHKERRQ(ierr);
            /* Populate operator here. */
            ierr = PCPatchComputeOperator(pc, mat, i); CHKERRQ(ierr);
            ierr = KSPSetOperators(patch->ksp[i], mat, mat);
            /* Drop reference so the KSPSetOperators below will blow it away. */
            ierr = MatDestroy(&mat); CHKERRQ(ierr);
        }
        ierr = PetscLogEventBegin(PC_Patch_Solve, pc, 0, 0, 0); CHKERRQ(ierr);
        ierr = KSPSolve(patch->ksp[i], patch->patchX[i], patch->patchY[i]); CHKERRQ(ierr);
        ierr = PetscLogEventEnd(PC_Patch_Solve, pc, 0, 0, 0); CHKERRQ(ierr);
        if (!patch->save_operators) {
            PC pc;
            ierr = KSPSetOperators(patch->ksp[i], NULL, NULL); CHKERRQ(ierr);
            ierr = KSPGetPC(patch->ksp[i], &pc); CHKERRQ(ierr);
            /* Destroy PC context too, otherwise the factored matrix hangs around. */
            ierr = PCReset(pc); CHKERRQ(ierr);
        }

        /* XXX: This bit needs changed for multiplicative combinations. */
        /* XXX: pef thinks "do we not need to weight these
         * contributions by the dof multiplicity?" */
        ierr = PCPatch_ScatterLocal_Private(pc, i + pStart,
                                            patch->patchY[i], patch->localY,
                                            ADD_VALUES, SCATTER_REVERSE); CHKERRQ(ierr);
    }
    /* Now patch->localY contains the solution of the patch solves, so
     * we need to combine them all.  This hardcodes an ADDITIVE
     * combination right now.  If one wanted multiplicative, the
     * scatter/gather stuff would have to be reworked a bit. */
    ierr = VecSet(y, 0.0); CHKERRQ(ierr);
    ierr = VecGetArrayRead(patch->localY, (const PetscScalar **)&localY); CHKERRQ(ierr);
    ierr = VecGetArray(y, &globalY); CHKERRQ(ierr);
    ierr = PetscSFReduceBegin(patch->defaultSF, patch->data_type, localY, globalY, MPI_SUM); CHKERRQ(ierr);
    ierr = PetscSFReduceEnd(patch->defaultSF, patch->data_type, localY, globalY, MPI_SUM); CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(patch->localY, (const PetscScalar **)&localY); CHKERRQ(ierr);
    if (patch->partition_of_unity) {
        ierr = VecRestoreArray(y, &globalY); CHKERRQ(ierr);
        ierr = VecPointwiseMult(y, y, patch->dof_weights); CHKERRQ(ierr);
        ierr = VecGetArray(y, &globalY); CHKERRQ(ierr);
    }

    /* Now we need to send the global BC values through */
    ierr = VecGetArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = ISGetSize(patch->bcNodes, &numBcs); CHKERRQ(ierr);
    ierr = ISGetIndices(patch->bcNodes, &bcNodes); CHKERRQ(ierr);
    ierr = VecGetLocalSize(x, &size); CHKERRQ(ierr);
    for ( PetscInt i = 0; i < numBcs; i++ ) {
        for ( PetscInt j = 0; j < patch->bs; j++ ) {
            const PetscInt idx = patch->bs * bcNodes[i] + j;
            if (idx < size) {
                globalY[idx] = globalX[idx];
            }
        }
    }
    ierr = ISRestoreIndices(patch->bcNodes, &bcNodes); CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(x, &globalX); CHKERRQ(ierr);
    ierr = VecRestoreArray(y, &globalY); CHKERRQ(ierr);
    ierr = PetscOptionsPopGetViewerOff(); CHKERRQ(ierr);
    ierr = PetscLogEventEnd(PC_Patch_Apply, pc, 0, 0, 0); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCSetUpOnBlocks_PATCH"
static PetscErrorCode PCSetUpOnBlocks_PATCH(PC pc)
{
  PC_PATCH           *patch = (PC_PATCH*)pc->data;
  PetscErrorCode      ierr;
  PetscInt            i;
  KSPConvergedReason  reason;

  PetscFunctionBegin;
  PetscFunctionReturn(0);
  for (i=0; i<patch->npatch; i++) {
    ierr = KSPSetUp(patch->ksp[i]); CHKERRQ(ierr);
    ierr = KSPGetConvergedReason(patch->ksp[i], &reason); CHKERRQ(ierr);
    if (reason == KSP_DIVERGED_PCSETUP_FAILED) {
      pc->failedreason = PC_SUBPC_ERROR;
    }
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCSetFromOptions_PATCH"
static PetscErrorCode PCSetFromOptions_PATCH(PetscOptionItems *PetscOptionsObject, PC pc)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscErrorCode  ierr;
    PetscBool       flg;
    char            sub_mat_type[256];

    PetscFunctionBegin;
    ierr = PetscOptionsHead(PetscOptionsObject, "Vertex-patch Additive Schwarz options"); CHKERRQ(ierr);

    ierr = PetscOptionsBool("-pc_patch_save_operators", "Store all patch operators for lifetime of PC?",
                            "PCPatchSetSaveOperators", patch->save_operators, &patch->save_operators, &flg); CHKERRQ(ierr);

    ierr = PetscOptionsBool("-pc_patch_partition_of_unity", "Weight contributions by dof multiplicity?",
                            "PCPatchSetPartitionOfUnity", patch->partition_of_unity, &patch->partition_of_unity, &flg); CHKERRQ(ierr);

    ierr = PetscOptionsFList("-pc_patch_sub_mat_type", "Matrix type for patch solves", "PCPatchSetSubMatType",MatList, NULL, sub_mat_type, 256, &flg); CHKERRQ(ierr);
    if (flg) {
        ierr = PCPatchSetSubMatType(pc, sub_mat_type); CHKERRQ(ierr);
    }
    ierr = PetscOptionsTail(); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCView_PATCH"
static PetscErrorCode PCView_PATCH(PC pc, PetscViewer viewer)
{
    PC_PATCH       *patch = (PC_PATCH *)pc->data;
    PetscErrorCode  ierr;
    PetscMPIInt     rank;
    PetscBool       isascii;
    PetscViewer     sviewer;
    PetscFunctionBegin;
    ierr = PetscObjectTypeCompare((PetscObject)viewer,PETSCVIEWERASCII,&isascii);CHKERRQ(ierr);

    ierr = MPI_Comm_rank(PetscObjectComm((PetscObject)pc),&rank);CHKERRQ(ierr);
    if (!isascii) {
        PetscFunctionReturn(0);
    }
    ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer, "Vertex-patch Additive Schwarz with %d patches\n", patch->npatch); CHKERRQ(ierr);
    if (!patch->save_operators) {
        ierr = PetscViewerASCIIPrintf(viewer, "Not saving patch operators (rebuilt every PCApply)\n"); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPrintf(viewer, "Saving patch operators (rebuilt every PCSetUp)\n"); CHKERRQ(ierr);
    }
    ierr = PetscViewerASCIIPrintf(viewer, "DM used to define patches:\n"); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
    if (patch->dm) {
        ierr = DMView(patch->dm, viewer); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPrintf(viewer, "DM not yet set.\n"); CHKERRQ(ierr);
    }
    ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer, "KSP on patches (all same):\n"); CHKERRQ(ierr);
    
    if (patch->ksp) {
        ierr = PetscViewerGetSubViewer(viewer, PETSC_COMM_SELF, &sviewer); CHKERRQ(ierr);
        if (!rank) {
            ierr = PetscViewerASCIIPushTab(sviewer); CHKERRQ(ierr);
            ierr = KSPView(patch->ksp[0], sviewer); CHKERRQ(ierr);
            ierr = PetscViewerASCIIPopTab(sviewer); CHKERRQ(ierr);
        }
        ierr = PetscViewerRestoreSubViewer(viewer, PETSC_COMM_SELF, &sviewer); CHKERRQ(ierr);
    } else {
        ierr = PetscViewerASCIIPushTab(viewer); CHKERRQ(ierr);
        ierr = PetscViewerASCIIPrintf(viewer, "KSP not yet set.\n"); CHKERRQ(ierr);
        ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    }

    ierr = PetscViewerASCIIPopTab(viewer); CHKERRQ(ierr);
    PetscFunctionReturn(0);
}

        
#undef __FUNCT__
#define __FUNCT__ "PCCreate_PATCH"
PETSC_EXTERN PetscErrorCode PCCreate_PATCH(PC pc)
{
    PetscErrorCode ierr;
    PC_PATCH       *patch;

    PetscFunctionBegin;

    ierr = PetscNewLog(pc, &patch); CHKERRQ(ierr);

    patch->sub_mat_type      = NULL;
    pc->data                 = (void *)patch;
    pc->ops->apply           = PCApply_PATCH;
    pc->ops->applytranspose  = 0; /* PCApplyTranspose_PATCH; */
    pc->ops->setup           = PCSetUp_PATCH;
    pc->ops->reset           = PCReset_PATCH;
    pc->ops->destroy         = PCDestroy_PATCH;
    pc->ops->setfromoptions  = PCSetFromOptions_PATCH;
    pc->ops->setuponblocks   = PCSetUpOnBlocks_PATCH;
    pc->ops->view            = PCView_PATCH;
    pc->ops->applyrichardson = 0;

    PetscFunctionReturn(0);
}
