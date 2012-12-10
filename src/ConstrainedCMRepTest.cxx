#include "IPOptProblemInterface.h"
#include "ScriptInterface.h"
#include "BasisFunctions2D.h"
#include "MedialAtom.h"
#include "CartesianMedialModel.h"
#include "OptimizationTerms.h"
#include "CoefficientMapping.h"
#include "MedialAtomGrid.h"
#include "PrincipalComponents.h"
#include "System.h"
#include "TestSolver.h"
#include "ITKImageWrapper.h"
#include "MedialModelIO.h"
#include "IpIpoptApplication.hpp"
#include "MedialAtomGrid.h"
#include "vtkPolyDataWriter.h"
#include "tetgen.h"

#include "vtkPolyData.h"
#include "vtkCellLocator.h"
#include "vtkSmartPointer.h"
#include <vtkPointLocator.h>

#include <vector>
#include <map>
#include <utility>
#include "itk_to_nifti_xform.h"

#include "ConstrainedCMRepObjectives.h"

void ExportMedialMeshToVTK(
    GenericMedialModel *xModel, ITKImageWrapper<float> *xImage, const char *file);

void ExportBoundaryMeshToVTK(
    GenericMedialModel *xModel, ITKImageWrapper<float> *xImage, const char *file);

// References to FORTRAN code
extern "C" {
void deflt_(int &alg, int *iv, int &liv, int &lv, double *v);

void sumsl_(
    int &n, double *d, double *x,
    void (*calcf)(int &, double *, int &, double &, int *, double *, void *),
    void (*calcg)(int &, double *, int &, double *, int *, double *, void *),
    int *iv, int &liv, int &lv, double *v,
    int *uiparm, double *urparm, void *ufparm);
}


// Reference to constants in Fortran code
const int mxiter_ = 18, mxfcal_ = 17, solprt_ = 22;

using namespace Ipopt;
using namespace gnlp;


typedef itk::Image<float, 3> ImageType;

class ClosestPointMatcher
{
public:
  typedef std::pair<int, SMLVec3d> PointMatch;

  ClosestPointMatcher(vtkPolyData *target, int nSamples);

  std::vector<SMLVec3d> FindClosestToTarget(VarVecArray &X);

  std::vector<PointMatch> FindClosestToSource(VarVecArray &X);

protected:
  vtkSmartPointer<vtkPolyData> m_Target;
  vtkSmartPointer<vtkCellLocator> m_TargetLocator;
  vtkSmartPointer<vtkPoints> m_ReducedTarget;
};

ClosestPointMatcher
::ClosestPointMatcher(vtkPolyData *target, int nSamples)
{
  // Store the target
  m_Target = target;

  // Create a locator
  m_TargetLocator = vtkSmartPointer<vtkCellLocator>::New();
  m_TargetLocator->SetDataSet(m_Target);
  m_TargetLocator->CacheCellBoundsOn();
  m_TargetLocator->BuildLocator();

  // Create a set of samples from the target
  // TODO: reimplement using quadric clustering!
  m_ReducedTarget = vtkSmartPointer<vtkPoints>::New();
  m_ReducedTarget->Allocate(nSamples);
  for(int i = 0; i < nSamples; i++)
    {
    int q = rand() % m_Target->GetNumberOfPoints();
    m_ReducedTarget->InsertNextPoint(m_Target->GetPoint(q));
    }
}

std::vector<SMLVec3d>
ClosestPointMatcher::FindClosestToTarget(VarVecArray &X)
{
  // Output vector
  std::vector<SMLVec3d> cp(X.size());

  // Compute thickness values
  for(int i = 0; i < X.size(); i++)
    {
    SMLVec3d xi = gnlp::VectorEvaluate(X[i]);

    double xs[3], d2, d;
    int subid;
    vtkIdType cellid;

    m_TargetLocator->FindClosestPoint(xi.data_block(), xs, cellid, subid, d2);

    cp[i] = SMLVec3d(xs);
    }

  return cp;
}

std::vector<ClosestPointMatcher::PointMatch>
ClosestPointMatcher::FindClosestToSource(VarVecArray &X)
{
  // Create a VTK points object
  vtkSmartPointer<vtkPoints> out_pts = vtkSmartPointer<vtkPoints>::New();
  out_pts->Allocate(X.size());

  for(int i = 0; i < X.size(); i++)
    {
    out_pts->InsertNextPoint(
          X[i][0]->Evaluate(), X[i][1]->Evaluate(), X[i][2]->Evaluate());
    }

  vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
  poly->SetPoints(out_pts);

  // Create locator for finding closest points
  vtkSmartPointer<vtkPointLocator> loc = vtkSmartPointer<vtkPointLocator>::New();
  loc->SetDataSet(poly);
  loc->BuildLocator();

  // Sample points from the target mesh
  std::vector<PointMatch> result;
  for(int i = 0; i < m_ReducedTarget->GetNumberOfPoints(); i++)
    {
    SMLVec3d xTarget(m_ReducedTarget->GetPoint(i));
    vtkIdType id = loc->FindClosestPoint(xTarget.data_block());
    result.push_back(std::make_pair((int) id, xTarget));
    }

  return result;
}

// Callback functions for TOMS
IPOptProblemInterface *globopt;
void toms_calcf(int &n, double *x, int &nf, double &f, int *dummy1, double *dummy2, void *info)
{
  globopt->eval_f(n, x, true, f);
}

void toms_calcg(int &n, double *x, int &nf, double *g, int *, double *, void *info)
{
  globopt->eval_grad_f(n, x, true, g);
}


void run_toms(IPOptProblemInterface *ip, ConstrainedNonLinearProblem *p)
{
  // SOLVE USING TOMS
  globopt = ip;
  int nCoeff = p->GetNumberOfVariables();
  double *scaling = (double *) malloc(nCoeff * sizeof(double));
  std::fill_n(scaling, nCoeff, 1.0);

  double *x = (double *) malloc(nCoeff * sizeof(double));
  for(int i = 0; i < nCoeff; i++)
    x[i] = p->GetVariableValue(i);

  // Specify the parameters to the sumsl_ routine
  int liv = 60, lv = 71+ nCoeff * (nCoeff+15) / 2;
  int *iv = (int *) malloc(liv * sizeof(int));
  double *v = (double *) malloc(lv * sizeof(double));

  std::fill_n(iv, liv, 0);
  std::fill_n(v, lv, 0.0);

  // Load the defaults
  int xAlg = 2;

  // Initialize the parameters of the method
  deflt_(xAlg, iv, liv, lv, v);
  iv[mxiter_ - 1] = 100;
  iv[mxfcal_ - 1] = 10 * 100;
  // iv[19 - 1] = 0;
  // iv[22 - 1] = 0;
  // iv[24 - 1] = 0;

  sumsl_(
        nCoeff, scaling, x,
        &toms_calcf, &toms_calcg,
        iv, liv, lv, v,
        NULL, NULL, NULL);

}

Expression *TetraHedronVolume(
    Problem *p,
    std::vector<Expression *> a,
    std::vector<Expression *> b,
    std::vector<Expression *> c,
    std::vector<Expression *> d)
{
  // Compute the three edge vectors
  std::vector<Expression *> Q(3, NULL), R(3, NULL), S(3, NULL);
  for(int j = 0; j < 3; j++)
    {
    Q[j] = new BinaryDifference(p, b[j], a[j]);
    R[j] = new BinaryDifference(p, c[j], a[j]);
    S[j] = new BinaryDifference(p, d[j], a[j]);
    }

  // Compute the vector triple product
  Expression *vol =
      new TernarySum(
        p,
        new BinaryProduct(
          p, Q[0], new BinaryDifference(
            p,
            new BinaryProduct(p, R[1], S[2]),
            new BinaryProduct(p, R[2], S[1]))),
        new BinaryProduct(
          p, Q[1], new BinaryDifference(
            p,
            new BinaryProduct(p, R[2], S[0]),
            new BinaryProduct(p, R[0], S[2]))),
        new BinaryProduct(
          p, Q[2], new BinaryDifference(
            p,
            new BinaryProduct(p, R[0], S[1]),
            new BinaryProduct(p, R[1], S[0]))));

  return vol;
}


double GetCentralDifference(Problem *p, Expression *ex, Variable *v, double delta=1e-5)
{
  double val = v->Evaluate();

  // Do the perturbation
  ex->MakeTreeDirty();
  v->SetValue(val + delta);
    double f2 = ex->Evaluate();

  ex->MakeTreeDirty();
  v->SetValue(val - delta);
  double f1 = ex->Evaluate();

  v->SetValue(val);

  return (f2 - f1) / (2 * delta);
}

void TestExpressionRandomDerivative(
    Problem *p,
    Expression *ex,
    const char *nickname,
    int order)
{
  // Check order
  if(order == 0)
    return;

  // Get the list of dependent variables
  const Problem::Dependency &depvar = p->GetDependentVariables(ex);
  if(depvar.size() == 0)
    return;

  // Pick a random dependent variable to test
  int q = rand() % depvar.size();
  Problem::Dependency::const_iterator it = depvar.begin();
  for(int p = 0; p < q; p++) ++it;
  Variable *v = *it;

  // Test with respect to this variable
  Expression *pd = p->GetPartialDerivative(ex, v);
  if(!pd)
    return;

  double dAnalytic = pd ? pd->Evaluate() : 0.0;
  double dCentralDiff = GetCentralDifference(p, ex, v);

  printf("D[%10s,%10s,%d]: %12.8f  %12.8f  %12.8f\n",
         nickname, v->GetName().c_str(), order,
         dAnalytic, dCentralDiff, std::fabs(dAnalytic-dCentralDiff));

  // Test higher order derivatives
  TestExpressionRandomDerivative(p, pd, nickname, order-1);
}

void DerivativeTest(ConstrainedNonLinearProblem *p, int nTests)
{
  p->MakeChildrenDirty();
  printf("TEST [%12s]: %12s  %12s  %12s\n",
         "Variable", "Analytic", "CentralDiff", "Delta");

  // The derivatives of the objective function
  for(int i = 0; i < nTests; i++)
    {
    TestExpressionRandomDerivative(p, p->GetObjective(), "obj", 2);
    }

  // Test th derivatives of the constraints
  for(int i = 0; i < nTests; i++)
    {
    // Pick a random constraint
    int iCon = rand() % p->GetNumberOfConstraints();
    Expression *con = p->GetConstraint(iCon);

    char buffer[16];
    sprintf(buffer, "Con_%d", iCon);

    TestExpressionRandomDerivative(p, con, buffer, 2);
    }

  p->MakeChildrenDirty();
}




#include "vtkFloatArray.h"
#include "vtkPointData.h"

void SaveSamples(std::vector<std::vector<Expression *> > sampleX,
                  std::vector<Expression *> sampleF,
                  const char *filename)
{
  vtkPoints *pts = vtkPoints::New();
  pts->Allocate(sampleX.size());

  vtkFloatArray *arr = vtkFloatArray::New();
  arr->SetNumberOfComponents(1);
  arr->Allocate(sampleX.size());

  for(int i = 0; i < sampleX.size(); i++)
    {
    pts->InsertNextPoint(sampleX[i][0]->Evaluate(),
                         sampleX[i][1]->Evaluate(),
                         sampleX[i][2]->Evaluate());
    arr->InsertNextTuple1(sampleF[i]->Evaluate());
    }

  vtkPolyData *poly = vtkPolyData::New();
  poly->SetPoints(pts);
  poly->GetPointData()->SetScalars(arr);

  vtkPolyDataWriter *writer = vtkPolyDataWriter::New();
  writer->SetInput(poly);
  writer->SetFileName(filename);
  writer->Update();

}

void SaveGradient(
    ConstrainedNonLinearProblem *p,
    std::vector<std::vector<Expression *> > X,
    Expression *f,
    const char *filename)
{
  vtkPoints *pts = vtkPoints::New();
  pts->Allocate(X.size());

  vtkFloatArray *arr = vtkFloatArray::New();
  arr->SetNumberOfComponents(3);
  arr->Allocate(X.size());
  arr->SetName("Gradient");

  for(int i = 0; i < X.size(); i++)
    {
    pts->InsertNextPoint(X[i][0]->Evaluate(),
                         X[i][1]->Evaluate(),
                         X[i][2]->Evaluate());
    Expression *dx = p->GetPartialDerivative(f, (Variable *)X[i][0]);
    Expression *dy = p->GetPartialDerivative(f, (Variable *)X[i][1]);
    Expression *dz = p->GetPartialDerivative(f, (Variable *)X[i][2]);
    arr->InsertNextTuple3(dx ? dx->Evaluate() : 0,
                          dy ? dy->Evaluate() : 0,
                          dz ? dz->Evaluate() : 0);
    }

  vtkPolyData *poly = vtkPolyData::New();
  poly->SetPoints(pts);
  poly->GetPointData()->SetScalars(arr);

  vtkPolyDataWriter *writer = vtkPolyDataWriter::New();
  writer->SetInput(poly);
  writer->SetFileName(filename);
  writer->Update();
}

#include "itkOrientedRASImage.h"
#include "itk_to_nifti_xform.h"
#include "vtkCubeSource.h"
#include "vtkTransform.h"
#include "vtkTransformFilter.h"
#include "vtkCleanPolyData.h"
#include "vtkCell.h"
#include "vtkSmartPointer.h"
#include "vtkTriangleFilter.h"
#include "vtkLinearSubdivisionFilter.h"

/**
  Generate a tetrahedral mesh for the model exerior using TetGen.
  */
void CreateTetgenMesh(GenericMedialModel *model,
                      FloatImage *image,
                      VarVecArray &X,
                      ConstrainedNonLinearProblem *problem)
{
  // Create a mesh object to populate
  tetgenio in;
  in.initialize();

  // Create the cube
  vtkSmartPointer<vtkCubeSource> cube = vtkCubeSource::New();
  cube->SetBounds(-0.5, 0.5 + image->GetInternalImage()->GetImageSize(0),
                  -0.5, 0.5 + image->GetInternalImage()->GetImageSize(1),
                  -0.5, 0.5 + image->GetInternalImage()->GetImageSize(2));

  // Subdivide the cube
  vtkSmartPointer<vtkTriangleFilter> fltTri = vtkTriangleFilter::New();
  fltTri->SetInputConnection(cube->GetOutputPort());

  vtkSmartPointer<vtkLinearSubdivisionFilter> fltSub = vtkLinearSubdivisionFilter::New();
  fltSub->SetInputConnection(fltTri->GetOutputPort());
  fltSub->SetNumberOfSubdivisions(2);

  // Get the transform matrix
  vnl_matrix_fixed<double, 4, 4> TS = ConstructNiftiSform(
        image->GetInternalImage()->GetInternalImage()->GetDirection().GetVnlMatrix(),
        image->GetInternalImage()->GetInternalImage()->GetOrigin().GetVnlVector(),
        image->GetInternalImage()->GetInternalImage()->GetSpacing().GetVnlVector());

  vtkSmartPointer<vtkCleanPolyData> clean = vtkCleanPolyData::New();
  clean->SetInputConnection(fltSub->GetOutputPort());

  vtkSmartPointer<vtkTransform> tran = vtkTransform::New();
  tran->SetMatrix(TS.data_block());

  // Create the transform filter
  vtkSmartPointer<vtkTransformFilter> tf = vtkTransformFilter::New();
  tf->SetInputConnection(clean  ->GetOutputPort());
  tf->SetTransform(tran);
  tf->Update();

  // Get the transformed cube
  vtkSmartPointer<vtkPolyData> tcube = dynamic_cast<vtkPolyData *>(tf->GetOutput());

  // Initialize all the points (number of points, plus six cube vertices)
  in.numberofpoints = model->GetNumberOfBoundaryPoints() + tcube->GetNumberOfPoints();
  in.pointlist = new REAL[in.numberofpoints * 3];

  in.pointmarkerlist = new int[in.numberofpoints];

  std::fill(in.pointmarkerlist,
            in.pointmarkerlist + model->GetNumberOfBoundaryPoints(), 1);
  std::fill(in.pointmarkerlist + model->GetNumberOfBoundaryPoints(),
            in.pointmarkerlist + in.numberofpoints, 2);

  /*
  in.numberofpointattributes = 1;
  in.pointattributelist = new REAL[in.numberofpoints];
  std::fill(in.pointattributelist,
            in.pointattributelist + model->GetNumberOfBoundaryPoints(), 1.0);
  std::fill(in.pointattributelist + model->GetNumberOfBoundaryPoints(),
            in.pointattributelist + in.numberofpoints, 2.0);
            */

  // Fill out the point array
  for(MedialBoundaryPointIterator bit = model->GetBoundaryPointIterator();
      !bit.IsAtEnd(); ++bit)
    {
    REAL *p = in.pointlist + 3 * bit.GetIndex();
    SMLVec3d X = GetBoundaryPoint(bit, model->GetAtomArray()).X;
    p[0] = X[0]; p[1] = X[1]; p[2] = X[2];
    }

  // Pass in the cube vertices
  REAL *p = in.pointlist + model->GetNumberOfBoundaryPoints() * 3;
  for(int i = 0; i < tcube->GetNumberOfPoints(); i++)
    {
    for(int j = 0; j < 3; j++)
      *(p++) = tcube->GetPoint(i)[j];
    }

  // Create the faces in the mesh
  in.numberoffacets = model->GetNumberOfBoundaryTriangles() + tcube->GetNumberOfCells();
  in.facetlist = new tetgenio::facet[in.numberoffacets];
  in.facetmarkerlist = new int[in.numberoffacets];

  const int TRIMARK = 100000;
  const int CUBMARK = 200000;

  // Initialize all of the facets
  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {
    // Set up the facet
    tetgenio::facet &f = in.facetlist[trit.GetIndex()];
    f.numberofpolygons = 1;
    f.numberofholes = 0;
    f.holelist = NULL;
    f.polygonlist = new tetgenio::polygon[1];
    f.polygonlist[0].numberofvertices = 3;
    f.polygonlist[0].vertexlist = new int[3];
    f.polygonlist[0].vertexlist[0] = trit.GetBoundaryIndex(0);
    f.polygonlist[0].vertexlist[1] = trit.GetBoundaryIndex(1);
    f.polygonlist[0].vertexlist[2] = trit.GetBoundaryIndex(2);

    // Pass the facet into list
    in.facetmarkerlist[trit.GetIndex()] = TRIMARK + trit.GetIndex();
    }

  // Initialize the cube's facets
  tetgenio::facet *fp = in.facetlist + model->GetNumberOfBoundaryTriangles();
  int *fmp = in.facetmarkerlist + model->GetNumberOfBoundaryTriangles();
  for(int i = 0; i < tcube->GetNumberOfCells(); i++)
    {
    vtkCell *cell = tcube->GetCell(i);
    fp->numberofpolygons = 1;
    fp->numberofholes = 0;
    fp->holelist = NULL;
    fp->polygonlist = new tetgenio::polygon[1];
    fp->polygonlist[0].numberofvertices = cell->GetNumberOfPoints();
    fp->polygonlist[0].vertexlist = new int[cell->GetNumberOfPoints()];
    for(int j = 0; j < cell->GetNumberOfPoints(); j++)
      {
      fp->polygonlist[0].vertexlist[j] =
          cell->GetPointId(j) + model->GetNumberOfBoundaryPoints();
      }
    *fmp++ = CUBMARK + i;
    ++fp;
    }

  // Add a hole for the volume
  /*
  in.numberofholes = 1;
  in.holelist = new double[3];
  in.holelist[0] = model->GetAtomArray()[0].X[0];
  in.holelist[1] = model->GetAtomArray()[0].X[1];
  in.holelist[2] = model->GetAtomArray()[0].X[2];
  */

  // Save the mesh
  in.save_nodes(const_cast<char *>("mytest"));
  in.save_poly(const_cast<char *>("mytest"));

  // Create an output mesh object
  tetgenio out;
  out.initialize();

  // Create the options
  tetgenbehavior tb;
  tb.parse_commandline(const_cast<char *>("-p -q3.2 -YY"));

  // Perform tetrahedralization
  tetrahedralize(&tb, &in, &out);

  // Now let's see what we got out
  printf("TETGEN result: %d tets, %d points\n",
         out.numberoftetrahedra,
         out.numberofpoints);

  // We need to convert the tetrahedralization into a set of variables and
  // constraints. There are three types of nodes, with attributes as follows:
  //   0  -  nodes inserted by tetgen on the interior.
  //   1  -  nodes in the model
  //   2  -  nodes in the cube

  // We want to create new variables for the 0-nodes. We create constants for
  // the coordinates of the 1-nodes.
  VarVecArray Y(out.numberofpoints, VarVec(3, NULL));
  for(int i = 0; i < out.numberofpoints; i++)
    {
    int type = out.pointmarkerlist[i];
    for(int j = 0; j < 3; j++)
      {
      if(type == 1)
        {
        // Medial mesh point - copy from X
        Y[i][j] = X[i][j];
        }
      else if(type == 2)
        {
        // Cube mesh point - create a new constant
        Y[i][j] = new Constant(problem, out.pointlist[i*3+j]);
        }
      else if(type == 0)
        {
        // Point created by tetgen
        Y[i][j] = problem->AddVariable("Yij", out.pointlist[i*3+j]);
        }
      }
    }

  // Finally, we create volume constraints for all the tetrahedra
  for(int i = 0; i < out.numberoftetrahedra; i++)
    {
    int *tet = out.tetrahedronlist + i * 4;

    Expression *vol =
        TetraHedronVolume(problem, Y[tet[0]], Y[tet[1]], Y[tet[2]], Y[tet[3]]);

    double tv = vol->Evaluate();

    problem->AddConstraint(vol, "TETVOL", 0.1 * tv, 100 * tv);
    }
}







void SaveCircumcenterMesh(VarVecArray &CC, VarVec &CR, VarVecArray &CCBC)
{

  vtkPoints *out_pts = vtkPoints::New();
  out_pts->Allocate(CC.size());

  vtkPolyData *out_poly = vtkPolyData::New();
  out_poly->SetPoints(out_pts);

  vtkFloatArray *arrRad = vtkFloatArray::New();
  arrRad->SetNumberOfComponents(1);
  arrRad->Allocate(CC.size());
  arrRad->SetName("Radius");

  vtkFloatArray *arrBC = vtkFloatArray::New();
  arrBC->SetNumberOfComponents(3);
  arrBC->Allocate(CC.size());
  arrBC->SetName("BC");

  for(int i = 0; i < CC.size(); i++)
    {
    out_pts->InsertNextPoint(
          CC[i][0]->Evaluate(), CC[i][1]->Evaluate(), CC[i][2]->Evaluate());

    arrRad->InsertNextTuple1(CR[i]->Evaluate());
    arrBC->InsertNextTuple3(
          CCBC[i][0]->Evaluate(),
          CCBC[i][1]->Evaluate(),
          CCBC[i][2]->Evaluate());
    }

  out_poly->GetPointData()->SetScalars(arrRad);
  out_poly->GetPointData()->AddArray(arrBC);

  vtkPolyDataWriter *writer = vtkPolyDataWriter::New();
  writer->SetInput(out_poly);
  writer->SetFileName("circumcenter.vtk");
  writer->Update();
}




/**
 * Compute edge and triangle properties (areas, normals, lengths) using
 * only quadratic expressions and constraints
 */
VarVecArray dummyArray;

void ComputeTriangleAndEdgeProperties(
    ConstrainedNonLinearProblem *p,
    TriangleMesh *mesh,
    const VarVecArray &X,   // Vertices
    VarVecArray &NT,        // Triangle normals
    VarVec &AT,             // Triangle areas
    double minArea,         // Smallest area allowed
    bool doEdges = false,   // Triangle edge lengths (opposite each vertex)
    VarVecArray &TEL = dummyArray)
{
  // Initialize to arrays of NULLS
  std::fill(AT.begin(), AT.end(), (Expression *) NULL);
  std::fill(NT.begin(), NT.end(), VarVec(3, NULL));
  AT.resize(mesh->triangles.size(), NULL);
  NT.resize(mesh->triangles.size(), VarVec(3, NULL));

  if(doEdges)
    {
    std::fill(TEL.begin(), TEL.end(), VarVec(3, NULL));
    TEL.resize(mesh->triangles.size(), VarVec(3, NULL));
    }

  // Iterate over all the triangles in this mesh
  for(int it = 0; it < mesh->triangles.size(); it++)
    {
    // Here is a triangle
    Triangle &t = mesh->triangles[it];
    size_t *v = t.vertices;

    // The un-normalized normal of the triangle
    VarVec Xu = VectorApplyPairwise<BinaryDifference>(p, X[v[1]], X[v[0]]);
    VarVec Xv = VectorApplyPairwise<BinaryDifference>(p, X[v[2]], X[v[0]]);
    VarVec Xu_cross_Xv = CrossProduct(p, Xu, Xv);

    // The area is half the norm of this cross product
    vnl_vector_fixed<double, 3> v_Xu_cross_Xv = VectorEvaluate(Xu_cross_Xv);
    double v_area = 0.5 * v_Xu_cross_Xv.magnitude();
    vnl_vector_fixed<double, 3> v_normal = 0.5 * v_Xu_cross_Xv / v_area;

    // Create the variables for the triangle area and normal
    AT[it] = p->AddVariable("AT", v_area, minArea);
    for(int d = 0; d < 3; d++)
      NT[it][d] = p->AddVariable("NT", v_normal[d]);

    // Create the constraint relating the area and the normal
    for(int d = 0; d < 3; d++)
      {
      Expression *con = new BinaryDifference(
            p,
            new ScalarProduct(p, new BinaryProduct(p, AT[it], NT[it][d]), 2.0),
            Xu_cross_Xv[d]);

      if(fabs(con->Evaluate()) > 1e-6)
        std::cout << "Con_TA-TN: " << con->Evaluate() << std::endl;

      p->AddConstraint(con, "TA-TN", 0, 0);
      }

    // Normal is length one
    Expression *normlen = DotProduct(p, NT[it], NT[it]);
    p->AddConstraint(normlen, "TN.TN", 1.0, 1.0);

    // Compute the edges
    if(doEdges)
      {
      for(int d = 0; d < 3; d++)
        {
        // The edge may have been set up by the opposite triangle
        if(TEL[it][d] != NULL)
          continue;

        // The opposite vertices
        size_t v1 = v[(d + 1) % 3], v2 = v[(d + 2) % 3];

        // Set up the egde length expression
        Expression *edgeLenSq = DistanceSqr(p, X[v1], X[v2]);

        // Create a variable for the edge length
        Expression *edgeLen = p->AddVariable("EL", sqrt(edgeLenSq->Evaluate()), 0);

        // Create the constraint linking the two
        Expression *con = new BinaryDifference(
              p, new Square(p, edgeLen), edgeLenSq);
        p->AddConstraint(con, "EDGELEN", 0, 0);

        // Assign the edge length variable to the current and opposite triangle
        TEL[it][d] = edgeLen;
        if(t.neighbors[d] != NOID)
          {
          TEL[t.neighbors[d]][t.nedges[d]] = edgeLen;
          }
        }
      }
    }
}

#include "BruteForceSubdivisionMedialModel.h"


Expression *ComputeDistanceToMeshObjective(
    ConstrainedNonLinearProblem *p,
    ClosestPointMatcher *cpm,
    VarVecArray &X)
{
  // For each point on the model, find the closest target point
  std::vector<SMLVec3d> targetPoint = cpm->FindClosestToTarget(X);

  BigSum *objSqDist = new BigSum(p);
  for(int i = 0; i < X.size(); i++)
    {
    for(int j = 0; j < 3; j++)
      {
      objSqDist->AddSummand(
            new Square(p,
                       new BinaryDifference(p, X[i][j],
                                            new Constant(p, targetPoint[i][j]))));
      }
    }

  return objSqDist;
}

Expression *ComputeDistanceToModelObjective(
    ConstrainedNonLinearProblem *p,
    ClosestPointMatcher *cpm,
    VarVecArray &X)
{
  std::vector<ClosestPointMatcher::PointMatch> meshToModel = cpm->FindClosestToSource(X);
  BigSum *objRecipSqDist = new BigSum(p);
  for(int i = 0; i < meshToModel.size(); i++)
    {
    SMLVec3d xMesh = meshToModel[i].second;
    int iModel = meshToModel[i].first;
    for(int j = 0; j < 3; j++)
      {
      objRecipSqDist->AddSummand(
            new Square(p,
                       new BinaryDifference(p, X[iModel][j],
                                            new Constant(p, xMesh[j]))));
      }
    }

  return objRecipSqDist;
}

void SaveBoundaryMesh(const char *file,
                      ConstrainedNonLinearProblem *p,
                      TriangleMesh *bmesh,
                      std::vector<int> &mIndex,
                      std::vector<std::vector<int> > &mtbIndex,
                      VarVecArray &X,
                      VarVecArray &N,
                      const VarVec &R)
{
  vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
  pts->Allocate(X.size());

  vtkSmartPointer<vtkFloatArray> rad = vtkSmartPointer<vtkFloatArray>::New();
  rad->SetNumberOfComponents(1);
  rad->Allocate(X.size());
  rad->SetName("Radius");

  vtkSmartPointer<vtkIntArray> mix = vtkSmartPointer<vtkIntArray>::New();
  mix->SetNumberOfComponents(1);
  mix->Allocate(X.size());
  mix->SetName("MedialIndex");

  vtkSmartPointer<vtkIntArray> mult = vtkSmartPointer<vtkIntArray>::New();
  mult->SetNumberOfComponents(1);
  mult->Allocate(X.size());
  mult->SetName("Tangency");

  vtkSmartPointer<vtkFloatArray> norm = vtkSmartPointer<vtkFloatArray>::New();
  norm->SetNumberOfComponents(3);
  norm->Allocate(X.size());

  for(int i = 0; i < X.size(); i++)
    {
    int j = mIndex[i];
    pts->InsertNextPoint(X[i][0]->Evaluate(), X[i][1]->Evaluate(), X[i][2]->Evaluate());
    norm->InsertNextTuple3(N[i][0]->Evaluate(), N[i][1]->Evaluate(), N[i][2]->Evaluate());
    rad->InsertNextTuple1(R[j]->Evaluate());
    mix->InsertNextTuple1(j);
    mult->InsertNextTuple1(mtbIndex[j].size());
    }

  vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
  pd->Allocate(bmesh->triangles.size());
  pd->SetPoints(pts);
  pd->GetPointData()->SetNormals(norm);
  pd->GetPointData()->AddArray(mix);
  pd->GetPointData()->AddArray(mult);
  pd->GetPointData()->AddArray(rad);

  for(int i = 0; i < bmesh->triangles.size(); i++)
    {
    vtkIdType vtx[3];
    for(int j = 0; j < 3; j++)
      vtx[j] = bmesh->triangles[i].vertices[j];
    pd->InsertNextCell(VTK_TRIANGLE, 3, vtx);
    }

  vtkSmartPointer<vtkPolyDataWriter> writer = vtkSmartPointer<vtkPolyDataWriter>::New();
  writer->SetInput(pd);
  writer->SetFileName(file);
  writer->Update();
}



void SaveMedialMesh(const char *file,
                    ConstrainedNonLinearProblem *p,
                    TriangleMesh *bmesh,
                    std::vector<int> &mIndex,
                    const VarVecArray &M,
                    const VarVec &R)
{
  vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
  pts->Allocate(M.size());

  vtkSmartPointer<vtkFloatArray> rad = vtkSmartPointer<vtkFloatArray>::New();
  rad->SetNumberOfComponents(1);
  rad->Allocate(M.size());
  rad->SetName("Radius");

  for(int i = 0; i < M.size(); i++)
    {
    pts->InsertNextPoint(M[i][0]->Evaluate(), M[i][1]->Evaluate(), M[i][2]->Evaluate());
    rad->InsertNextTuple1(R[i]->Evaluate());
    }

  vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
  pd->Allocate(bmesh->triangles.size());
  pd->SetPoints(pts);
  pd->GetPointData()->SetScalars(rad);

  for(int i = 0; i < bmesh->triangles.size(); i++)
    {
    vtkIdType vtx[3];
    for(int j = 0; j < 3; j++)
      vtx[j] = mIndex[bmesh->triangles[i].vertices[j]];
    pd->InsertNextCell(VTK_TRIANGLE, 3, vtx);
    }

  vtkSmartPointer<vtkPolyDataWriter> writer = vtkSmartPointer<vtkPolyDataWriter>::New();
  writer->SetInput(pd);
  writer->SetFileName(file);
  writer->Update();
}


int main(int argc, char *argv[])
{
  // We want to load a triangular mesh in which each point is associated with
  // a tag (corresponding medial atom).

  // The first parameter is the cm-rep to start from
  char *cmrepfile = argv[1];

  // The second is the VTK mesh for ICP
  char *targetmesh = argv[2];

  // The image to fit to
  // char *targetimage = argv[3];

  // Load and process the mrep (TODO: remove)
  MedialPDE mrep(cmrepfile);
  SubdivisionMedialModel *tmpmodel =
      dynamic_cast<SubdivisionMedialModel *>(mrep.GetMedialModel());

  // We will restrict our operations to a boundary triangle mesh and a set of medial
  // atom indices for each point on the triangle mesh
  TriangleMesh *bmesh = tmpmodel->GetIterationContext()->GetBoundaryMesh();

  // Get the number of boundary points
  int nb = bmesh->nVertices;

  // Initialize the data we are extracting from the boundary mesh
  std::vector<int> mIndex(nb);
  std::vector<SMLVec3d> xInput(nb), nInput(nb);
  std::vector<double> rInput(nb);
  for(MedialBoundaryPointIterator it = tmpmodel->GetBoundaryPointIterator();
      !it.IsAtEnd(); ++it)
    {
    mIndex[it.GetIndex()] = it.GetAtomIndex();
    xInput[it.GetIndex()] = GetBoundaryPoint(it, tmpmodel->GetAtomArray()).X;
    }

  // At this point, we are only working with bMesh and mIndex and xInput


  // Get the number of medial points. TODO: we need to make sure that every
  // index between 0 and nm is represented.
  int nm = 1 + *std::max_element(mIndex.begin(), mIndex.end());

  // Create a list of boundary atoms for each medial atom
  typedef std::vector<std::vector<int> > MedialToBoundaryIndex;
  MedialToBoundaryIndex mtbIndex(nm);
  for(int i = 0; i < nb; i++)
    {
    mtbIndex[mIndex[i]].push_back(i);
    }


  // Load the target mesh
  vtkPolyData *target = ReadVTKMesh(targetmesh);

  // Load the target image
  /*
  BinaryImage imgBinary;
  imgBinary.LoadFromFile(targetimage);

  // Smooth and turn into floating point
  FloatImage *image = new FloatImage();
  image->SetToBlurredBinary(&imgBinary, 0.2);
  image->SetOutsideValue(-1.0);

  // The new jet interpolator
  ImageJetInterpolator interp_new;
  interp_new.SetInputImage(image->GetInternalImage()->GetInternalImage(), -1.0);

  // Test the interpolator
  SMLVec3d xProbe(-69.6316, -118.364, 56.4);

  ImageJetInterpolator::Vec3 gNew;
  ImageJetInterpolator::Mat3 hNew;
  double newval;
  interp_new.EvaluateAtPhysicalPoint(xProbe, newval, gNew, hNew);
  printf("New value %f, gradient [%f %f %f]\n", newval, gNew[0], gNew[1], gNew[2]);

  for(int i = 0; i < 3; i++)
    {
    SMLVec3d eps(0, 0, 0); eps[i] = 0.001;
    ImageJetInterpolator::Vec3 g1, g2;
    ImageJetInterpolator::Mat3 hDummy;
    double f1, f2;
    interp_new.EvaluateAtPhysicalPoint(xProbe+eps, f2, g2, hDummy);
    interp_new.EvaluateAtPhysicalPoint(xProbe-eps, f1, g1, hDummy);
    printf("Dx[%d]:   A = %f   N = %f\n", i, gNew[i], (f2-f1) / 0.002);

    for(int j = 0; j < 3; j++)
      printf("DDx[%d][%d]:   A = %f   N = %f\n", i, j, hNew[i][j], (g2[j]-g1[j]) / 0.002);
    }
    */

  // Create the optimization problem
  ConstrainedNonLinearProblem *p = new ConstrainedNonLinearProblem();

  // The boundary positions
  VarVecArray X(nb, VarVec(3, NULL));

  // The medial positions
  VarVecArray M(nm, VarVec(3, NULL));

  // The radius values
  VarVec R(nm, NULL);

  // The boundary normal vectors
  VarVecArray N(nb, VarVec(3, NULL));

  // The vectors U from the medial axis to the boundary. They are the same
  // as -R*N. They appear in multiple places though, so we should store them
  VarVecArray U(nb, VarVec(3, NULL));

  // The triangle areas, normals and edge lengths on the boundary surface
  // and on the medial surface (to avoid square roots)
  VarVec taX, taM;
  VarVecArray NT_X, TEL_X, NT_M;

  // A buffer for making variable names
  char buffer[64];

  // ------------------------------------------------------------------------
  // Configure the boundary point variables
  // ------------------------------------------------------------------------

  // Configure the medial point variables
  for(int i = 0; i < nb; i++)
    {
    SMLVec3d x = xInput[i];

    // Set up the medial coordinate and normal
    for(int j = 0; j < 3; j++)
      {
      sprintf(buffer, "X[%d,%d]", i, j);
      X[i][j] = p->AddVariable(buffer, x[j]);
      }
    }

  // ------------------------------------------------------------------------
  // Configure the constraints on the boundary normal
  // ------------------------------------------------------------------------

  // Create a LoopScheme for specifying normal vector constraints
  LoopTangentScheme lts;
  lts.SetMesh(bmesh);

  // Expressions describing Xu, Xv, Nu, Nv at each point on the boundary
  VarVecArray Xd[] = { VarVecArray(nb, VarVec(3, NULL)), VarVecArray(nb, VarVec(3, NULL)) };
  VarVecArray Nd[] = { VarVecArray(nb, VarVec(3, NULL)), VarVecArray(nb, VarVec(3, NULL)) };

  // Expression for the first fundamental and the shape operator
  VarVec curv_mean(nb, NULL);
  VarVec curv_gauss(nb, NULL);
  VarVec curv_k1(nb, NULL);
  VarVec curv_k2(nb, NULL);

  // Add the constraints relating each normal to the neighboring vertices
  for(int i = 0; i < nb; i++)
    {
    // Repeat for u and v directions
    for(int d = 0; d < 2; d++)
      {
      BigSum *Xdi[] = { new BigSum(p), new BigSum(p), new BigSum(p) };

      double wi = lts.GetOwnWeight(d, i);
      for(int j = 0; j < 3; j++)
        {
        Xdi[j]->AddSummand(new ScalarProduct(p, X[i][j], wi));
        }

      for(EdgeWalkAroundVertex walk(bmesh, i); !walk.IsAtEnd(); ++walk)
        {
        double wij = lts.GetNeighborWeight(d, walk);
        for(int j = 0; j < 3; j++)
          {
          Xdi[j]->AddSummand(new ScalarProduct(p, X[walk.MovingVertexId()][j], wij));
          }
        }

      // Store these expressions
      for(int j = 0; j < 3; j++)
        {
        Xd[d][i][j] = Xdi[j];
        }
      }

    // Compute the initial value of the normal
    SMLVec3d v_xu = VectorEvaluate(Xd[0][i]);
    SMLVec3d v_xv = VectorEvaluate(Xd[1][i]);
    SMLVec3d v_n = vnl_cross_3d(v_xu, v_xv).normalize();

    // Create the variables for the normal
    for(int j = 0; j < 3; j++)
      {
      sprintf(buffer, "N[%d,%d]", i, j);
      N[i][j] = p->AddVariable(buffer, v_n[j]);
      }

    // Create the constraints on the normal
    for(int d = 0; d < 2; d++)
      {
      // Create the constrait Xu . N = 0
      Expression *constrNormXu = DotProduct(p, Xd[d][i], N[i]);

      // Add the constraint to the problem
      p->AddConstraint(constrNormXu, "N.Xu", 0, 0);
      }

    // Add the constraint on the normal being norm 1
    Expression *constrNormMag = MagnitudeSqr(p, N[i]);
    p->AddConstraint(constrNormMag, "N.N", 1.0, 1.0);
    }


  // ------------------------------------------------------------------------
  // Configure the constraints relating boundary curvature to the radius fn
  // ------------------------------------------------------------------------

  for(int i = 0; i < nb; i++)
    {

    // For non-edge atoms, we are done, no need to compute the rest
    if(mtbIndex[mIndex[i]].size() > 1)
      {
      curv_mean[i] = new Constant(p, 0);
      curv_gauss[i] = new Constant(p, 0);
      curv_k1[i] = new Constant(p, 0);
      curv_k2[i] = new Constant(p, 0);
      continue;
      }

    // Compute the expressions Nu, Nv
    // Repeat for u and v directions
    for(int d = 0; d < 2; d++)
      {
      BigSum *Ndi[] = { new BigSum(p), new BigSum(p), new BigSum(p) };

      double wi = lts.GetOwnWeight(d, i);
      for(int j = 0; j < 3; j++)
        {
        Ndi[j]->AddSummand(new ScalarProduct(p, N[i][j], wi));
        }

      for(EdgeWalkAroundVertex walk(bmesh, i); !walk.IsAtEnd(); ++walk)
        {
        double wij = lts.GetNeighborWeight(d, walk);
        for(int j = 0; j < 3; j++)
          {
          Ndi[j]->AddSummand(new ScalarProduct(p, N[walk.MovingVertexId()][j], wij));
          }
        }

      // Store these expressions
      for(int j = 0; j < 3; j++)
        {
        Nd[d][i][j] = Ndi[j];
        }
      }

    // Compute the first fundamental form at the point.
    vnl_matrix_fixed<double, 2, 2> mFF1, mFF2, mSO;
    Expression *FF1[2][2], *FF2_neg[2][2], *SO[2][2];
    for(int q = 0; q < 2; q++)
      {
      for(int r = 0; r < 2; r++)
        {
        // Add the expression for the first f. f. as a constrained variable
        FF1[q][r] = p->AddExpressionAsConstrainedVariable(DotProduct(p, Xd[q][i], Xd[r][i]), "FF1");
        mFF1[q][r] = FF1[q][r]->Evaluate();

        // Minus the second f.f.
        FF2_neg[q][r] = DotProduct(p, Xd[q][i], Nd[r][i]);
        mFF2[q][r] = FF2_neg[q][r]->Evaluate();
        }
      }

    // Numerically solve for the shape operator
    mSO = - vnl_inverse(mFF1) * mFF2;

    // Add the expression for the shape operator
    for(int q = 0; q < 2; q++)
      {
      for(int r = 0; r < 2; r++)
        {
        SO[q][r] = p->AddVariable("SO", mSO[q][r]);
        }
      }

    // Solve for the shape operator
    for(int q = 0; q < 2; q++)
      {
      for(int r = 0; r < 2; r++)
        {
        // Minus the second f.f.
        Expression *sff = DotProduct(p, Xd[q][i], Nd[r][i]);

        // Expression for FFF * S - SFF
        Expression *con = new TernarySum(
              p,
              new BinaryProduct(p, FF1[q][0], SO[0][r]),
              new BinaryProduct(p, FF1[q][1], SO[1][r]),
              sff);

        if(fabs(con->Evaluate()) > 1e-6)
          std::cout << "Con_SO = " << con->Evaluate() << std::endl;

        // Set as a hard constraint
        p->AddConstraint(con, "SO", 0, 0);
        }
      }

    // Numerically solve for k1
    double mH = vnl_trace(mSO) / 2;
    double mK = vnl_det(mSO);
    double mk1 = mH - sqrt(mH*mH - mK);

    // Solve the characteristic polynomial for kappa1
    Expression *k1 = p->AddVariable("k1", mk1);

    // The constraint on kappa1
    Expression * con = new BinaryDifference(p,
                                            new BinaryProduct(p,
                                                              new BinaryDifference(p, SO[0][0], k1),
                                                              new BinaryDifference(p, SO[1][1], k1)),
                                            new BinaryProduct(p, SO[0][1], SO[1][0]));

    // Evaluate the constraint
    p->AddConstraint(con, "Kappa-eq", 0, 0);

    if(fabs(con->Evaluate()) > 1e-6)
      std::cout << "Con_K1 = " << con->Evaluate() << std::endl;

    // We want kappa1 to be the larger in magnitude of the two curvatures. This is equivalent
    // to having kappa1 > trace(SO)/2. K1 is negative.
    Expression *H = new ScalarProduct(p, new BinarySum(p, SO[0][0], SO[1][1]), 0.5);
    Expression *con2 = new BinaryDifference(p, k1, H);

    p->AddConstraint(con2, "Kappa-ineq", ConstrainedNonLinearProblem::LBINF, 0);

    if(con2->Evaluate() > -1e-6)
      std::cout << "Con_K1_sign = " << con2->Evaluate() << std::endl;

    // Store these curvatures for future reference
    curv_mean[i] = H;
    curv_k1[i] = k1;
    curv_k2[i] = new BinaryDifference(p, new ScalarProduct(p, H, 2), k1);
    curv_gauss[i] = new BinaryProduct(p, k1, curv_k2[i]);

    // Create the radius variable for this node
    int iAtom = mIndex[i];
    sprintf(buffer, "R[%d]", iAtom);
    R[iAtom] = p->AddVariable(buffer, -1.0 / k1->Evaluate(), 0.1);

    // Now enforce the link between R and K1
    Expression *conR = new BinaryProduct(p, R[iAtom], k1);
    p->AddConstraint(conR, "R*kappa", -1.0, -1.0);

    // Initialize the medial axis position
    for(int j = 0; j < 3; j++)
      {
      sprintf(buffer, "M[%d,%d]", iAtom, j);
      double mval = X[i][j]->Evaluate() - R[iAtom]->Evaluate() * N[i][j]->Evaluate();
      M[iAtom][j] = p->AddVariable(buffer, mval);
      }
    }

  // ------------------------------------------------------------------------
  // Compute the initial medial atoms and radii for non-edge atoms
  // ------------------------------------------------------------------------

  for(int i = 0; i < nm; i++)
    {
    int k = mtbIndex[i].size();
    if(k > 1)
      {
      // For atoms that are bi-tangent or greater, we find R and M that
      // minimize the expression Sum_j ||Xj - r Nj - M)||^2, i.e, the radius
      // such that the medial atoms computed for each boundary atom idependently
      // are as close together as possible. The r is found as
      // r = (Sum_{ij} (Xi-Xj)^t N_i) / (k^2 - Sum_{ij} Nj^t N_i)
      double numerator = 0.0, denominator = k*k;
      SMLVec3d sumX(0.0, 0.0, 0.0), sumN(0.0, 0.0, 0.0);
      for(int q = 0; q < k; q++)
        {
        int iq = mtbIndex[i][q];
        SMLVec3d Xq = VectorEvaluate(X[iq]), Nq = VectorEvaluate(N[iq]);
        for(int p = 0; p < k; p++)
          {
          int ip = mtbIndex[i][p];
          SMLVec3d Xp = VectorEvaluate(X[ip]), Np = VectorEvaluate(N[ip]);

          numerator += dot_product(Xq - Xp, Nq);
          denominator -= dot_product(Np, Nq);
          }

        sumX += Xq; sumN += Nq;
        }

      // Compute the best fit r and m
      double v_r = numerator / denominator;
      SMLVec3d v_m = (sumX - sumN * v_r) / ((double) k);

      // Store those values
      sprintf(buffer, "R[%d]", i);
      R[i] = p->AddVariable(buffer, v_r, 0.1);

      // Initialize the medial axis position
      for(int j = 0; j < 3; j++)
        {
        sprintf(buffer, "M[%d,%d]", i, j);
        M[i][j] = p->AddVariable(buffer, v_m[j]);
        }
      }
    }

  // ------------------------------------------------------------------------
  // Export the medial atom mesh for debugging purposes.
  // ------------------------------------------------------------------------
  SaveMedialMesh("medial_before.vtk", p, bmesh, mIndex, M, R);

  // ------------------------------------------------------------------------
  // Add the actual medial constraints
  // ------------------------------------------------------------------------

  for(int iBnd = 0; iBnd < nb; iBnd++)
    {
    int iAtom = mIndex[iBnd];

    // Code up X - r * N - M = 0
    for(int j = 0; j < 3; j++)
      {
      Expression *constMedial =
          new BinaryDifference(p,
                               X[iBnd][j],
                               new BinarySum(p,
                                             M[iAtom][j],
                                             new BinaryProduct(p,
                                                               R[iAtom],
                                                               N[iBnd][j])));
      p->AddConstraint(constMedial, "X-rNM", 0, 0);
      }
    }



  // ------------------------------------------------------------------------
  // Configure the boundary and medial triangle area variables
  // ------------------------------------------------------------------------

  // Compute boundary triangle edges, normals, and areas
  ComputeTriangleAndEdgeProperties(
        p, bmesh,
        X, NT_X, taX, 0.1, true, TEL_X);

  // Create a medial mesh that duplicates the boundary mesh
  TriangleMesh* mmesh = new TriangleMesh(*bmesh);

  // Change the triangle vertices to use m_index
  for(int i = 0; i < mmesh->triangles.size(); i++)
    {
    for(int j = 0; j < 3; j++)
      {
      mmesh->triangles[i].vertices[j] = mIndex[bmesh->triangles[i].vertices[j]];
      }
    }

  // Compute medial triangle edges, normals, and areas.
  // TODO: this is redundant, as it computes two sets of equal normals and
  // areas for each medial triangle, one facing each boundary triangle. We
  // could avoid this by checking for opposite triangles.
  ComputeTriangleAndEdgeProperties(
        p, mmesh,
        M, NT_M, taM, 0.1);



  // ------------------------------------------------------------------------
  // Define the objective on the basis
  // ------------------------------------------------------------------------

  // Define a basis for the surface
  int nBasis = 20;
  MeshBasisCoefficientMapping basismap_X(bmesh, nBasis, 3);

  // Define a basis for the medial axis
  // TODO: to what extent is this a valid basis - we need to visualize!
  MeshBasisCoefficientMapping basismap_M(bmesh, nBasis, 4);

  // Create the coefficient variables
  VarVecArray XC(nBasis, VarVec(3, NULL));
  VarVecArray MC(nBasis, VarVec(4, NULL));
  for(int i = 0; i < nBasis; i++)
    {
    for(int j = 0; j < 3; j++)
      {
      XC[i][j] = p->AddVariable("XC", 0.0);
      }
    for(int j = 0; j < 4; j++)
      {
      MC[i][j] = p->AddVariable("MC", 0.0);
      }
    }

  // Define the objective on the basis
  BigSum *objBasisResidual = new BigSum(p);
  for(int iBnd = 0; iBnd < nb; iBnd++)
    {
    SMLVec3d Xfixed = xInput[iBnd];

    for(int j = 0; j < 3; j++)
      {
      BigSum *xfit = new BigSum(p);

      xfit->AddSummand(new Constant(p, Xfixed[j]));
      for(int i = 0; i < nBasis; i++)
        {
        xfit->AddSummand(new ScalarProduct(p, XC[i][j],
                                           basismap_X.GetBasisComponent(i, iBnd)));
        }

      // Xfit is the approximation of X using the basis
      objBasisResidual->AddSummand(
            new Square(p, new BinaryDifference(p, xfit, X[iBnd][j])));
      }
    }

  // TODO: for the time being, I took out the medial residual computation,
  // because it is unreasonable to compute it as a difference from the initial
  // state, since the initial state is likely to be bogus. Need to revive this
  // later
  /*
  for(MedialAtomIterator it = model->GetAtomIterator();
      !it.IsAtEnd(); ++it)
    {
    int iatm = it.GetIndex();

    MedialAtom &a = model->GetAtomArray()[iatm];

    for(int j = 0; j < 4; j++)
      {
      double xfixed = (j < 3) ? a.X[j] : a.R;
      Expression *xdata = (j < 3) ? M[iatm][j] : R[iatm];

      BigSum *xfit = new BigSum(p);

      xfit->AddSummand(new Constant(p, xfixed));
      for(int i = 0; i < nBasis; i++)
        {
        xfit->AddSummand(new ScalarProduct(p, MC[i][j],
                                           basismap_M.GetBasisComponent(i, iatm)));
        }

      // Xfit is the approximation of X using the basis
      objBasisResidual->AddSummand(
            new Square(p, new BinaryDifference(p, xfit, xdata)));
      }
    }
  */

  // ------------------------------------------------------------------------
  // Create a total volume objective -
  // ------------------------------------------------------------------------
  BigSum *objVolume = new BigSum(p);

  // Measure the wedge volume for each boundary triangle
  VarVec wedgeVol(bmesh->triangles.size(), NULL);

  // Add the Jacobian constraints - all tetrahedra must have positive volume
  for(int i = 0; i < bmesh->triangles.size(); i++)
    {
    Triangle &T = bmesh->triangles[i];

    // Get the boundary vertices
    VarVec x[] = { X[T.vertices[0]], X[T.vertices[1]], X[T.vertices[2]] };

    // And the medial vertices
    VarVec m[] = { M[mIndex[T.vertices[0]]],
                   M[mIndex[T.vertices[1]]],
                   M[mIndex[T.vertices[2]]] };

    // There are several ways to cut a wedge into tetras, we choose one
    Expression *c1 = TetraHedronVolume(p, m[2], x[0], x[1], x[2]);
    Expression *c2 = TetraHedronVolume(p, m[1], x[0], x[1], m[2]);
    Expression *c3 = TetraHedronVolume(p, m[2], x[0], m[0], m[1]);

    // printf("Tetra Vols: %f, %f, %f\n", c1->Evaluate(), c2->Evaluate(), c3->Evaluate());

    // Save the total wedge volume for use in integration
    wedgeVol[i] = new TernarySum(p, c1, c2, c3);

    // Each tetra should have positive volume
    /*
    p->AddConstraint(c1, 0.1, 40);
    p->AddConstraint(c2, 0.1, 40);
    p->AddConstraint(c3, 0.1, 40);
    */

    // Total volume integral
    objVolume->AddSummand(wedgeVol[i]);
    }


  // ------------------------------------------------------------------------
  // Create the medial/boundary Jacobian constraint (normals point in same direction)
  // ------------------------------------------------------------------------
  double constJacFact = 0.1;
  for(int i = 0; i < bmesh->triangles.size(); i++)
    {
    // For this constraint, we just want the medial triangle normal and
    // the boundary triangle normal to point in the same direction!
    Expression *dp = DotProduct(p, NT_X[i], NT_M[i]);

    // Depending on the side, constrain up or down
    if(dp->Evaluate() < constJacFact)
      std::cout << "Bad Jacobian constraint: " << dp->Evaluate()
                << " in triangle " << i << std::endl;

    // Add the constraint
    p->AddConstraint(dp, "Jac", constJacFact, ConstrainedNonLinearProblem::UBINF);
    }


#ifdef OLD_CODE
  // Lower bound for Nm . Nx / (Nm . Nm)
  double constJacFact = 0.1;

  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {
    // Get the boundary vertices
    VarVec x[] = { X[trit.GetBoundaryIndex(0)],
                   X[trit.GetBoundaryIndex(1)],
                   X[trit.GetBoundaryIndex(2)] };

    // And the medial vertices
    VarVec m[] = { M[trit.GetAtomIndex(0)],
                   M[trit.GetAtomIndex(1)],
                   M[trit.GetAtomIndex(2)] };

    // Get the expression for the triangle normal
    VarVec xu = VectorApplyPairwise<BinaryDifference>(p, x[1], x[0]);
    VarVec xv = VectorApplyPairwise<BinaryDifference>(p, x[2], x[0]);
    VarVec Nx = CrossProduct(p, xu, xv);

    VarVec mu = VectorApplyPairwise<BinaryDifference>(p, m[1], m[0]);
    VarVec mv = VectorApplyPairwise<BinaryDifference>(p, m[2], m[0]);
    VarVec Nm = CrossProduct(p, mu, mv);

    // Get the dot products
    Expression *NmNx = DotProduct(p, Nx, Nm);
    Expression *NmNm = DotProduct(p, Nm, Nm);

    // Get the Jacobian constraint expression
    Expression *con = new BinaryDifference(p, NmNx,
                                           new ScalarProduct(p, NmNm, constJacFact));

    // Set the constraint
    p->AddConstraint(con, 0.0, ConstrainedNonLinearProblem::UBINF);

    // We can also constrain the size of the medial triangle from below
    p->AddConstraint(NmNm, 0.1, ConstrainedNonLinearProblem::UBINF);

    // Print the ratio
    // printf("JACCON: %f %f \n", NmNm->Evaluate(), NmNx->Evaluate() / NmNm->Evaluate());

    } 
#endif

  // ------------------------------------------------------------------------
  // Create the MIB constraint
  // ------------------------------------------------------------------------
  for(int iBnd = 0; iBnd < nb; iBnd++)
    {
    int iAtom = mIndex[iBnd];

    for(EdgeWalkAroundVertex walk(bmesh, iBnd); !walk.IsAtEnd(); ++walk)
      {
      int k = walk.MovingVertexId();
      Expression *distsq = DistanceSqr(p, M[iAtom], X[k]);
      p->AddConstraint(
            new BinaryDifference(p, distsq, new Square(p, R[iAtom])),
            "MIB", 0, ConstrainedNonLinearProblem::UBINF);
      }
    }



  // ------------------------------------------------------------------------
  // Construct the total surface area objective
  // ------------------------------------------------------------------------
  BigSum *objSurfArea = new BigSum(p);
  for(int i = 0; i < bmesh->triangles.size(); i++)
    {
    // Add the area to the objective
    objSurfArea->AddSummand(taX[i]);
    }

  // ------------------------------------------------------------------------
  // Solve for the circumcenter and circumradius of each boundary triangle
  // ------------------------------------------------------------------------

#ifdef CIRCUMCENTER

  // Define arrays for circumcenter, circumradius, barycentric coords of the c.c.
  VarVecArray CC(model->GetNumberOfBoundaryTriangles(), VarVec(3, NULL));
  VarVecArray CCBC(model->GetNumberOfBoundaryTriangles(), VarVec(3, NULL));
  VarVec CR(model->GetNumberOfBoundaryTriangles(), NULL);

  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {

    // Get the corners of the triangle
    VarVec XT[3] = {
      X[trit.GetBoundaryIndex(0)],
      X[trit.GetBoundaryIndex(1)],
      X[trit.GetBoundaryIndex(2)]
    };



    // We define the barycentric coordinates of the circumcenter, the circum
    // center itself and the circumradius using implicit equations, arising
    // from solving the constrained problem R=argmin(R^2) subj to. |X-C|^2 = R,
    // |Y-C|^2 = R, |Z-C|^2 = R, using Lagrange multipliers
    VarVec &lambda = CCBC[trit.GetIndex()];
    VarVec &C = CC[trit.GetIndex()];

    // Expressions for squared edge lengths
    VarVec elen2(3, NULL);
    elen2[0] = DistanceSqr(p, XT[1], XT[2]);
    elen2[1] = DistanceSqr(p, XT[2], XT[0]);
    elen2[2] = DistanceSqr(p, XT[0], XT[1]);

    // Sum of the edge lengths squared
    double elen2sum = elen2[0]->Evaluate() + elen2[1]->Evaluate() + elen2[2]->Evaluate();

    // Compute what the barycenter coordinates should be (Wikipedia)
    vnl_vector_fixed<double, 3> lambdaVal;
    for(int j = 0; j < 3; j++)
      lambdaVal[j] = elen2[j]->Evaluate() * (elen2sum - 2 * elen2[j]->Evaluate());
    lambdaVal /= lambdaVal.sum();

    // We constrain lambdas to be positive - forces acute triangles! And
    // probably setting an upper limit would work to constrain the minimal
    // angle of the triangle
    for(int j = 0; j < 3; j++)
      {
      // Wikipedia expression for the lambdas
      lambda[j] = p->AddVariable("l", lambdaVal[j], 0);
      }

    // Now we define the seven constraints that tie these variables.

    // L1 + L2 + L3 = 1
    p->AddConstraint(new TernarySum(p, lambda[0], lambda[1], lambda[2]), 1.0, 1.0);

    // C = L1 X + L2 Y + L3 Z
    for(int j = 0; j < 3; j++)
      {
      // What C should equal
      Expression *rhs = new TernarySum(p,
                                       new BinaryProduct(p, lambda[0], XT[0][j]),
                                       new BinaryProduct(p, lambda[1], XT[1][j]),
                                       new BinaryProduct(p, lambda[2], XT[2][j]));
      // Add the variable and the constraint
      C[j] = p->AddExpressionAsConstrainedVariable(rhs);
      }

    // Compute squared distances from the vertices to the center
    VarVec XCd2(3, NULL);
    XCd2[0] = DistanceSqr(p, XT[0], C);
    XCd2[1] = DistanceSqr(p, XT[1], C);
    XCd2[2] = DistanceSqr(p, XT[2], C);

    // Create radius variable
    Expression *Rc = CR[trit.GetIndex()] = p->AddVariable("Rc", sqrt(XCd2[0]->Evaluate()), 0);

    // |C-X|^2 = R^2
    for(int j = 0; j < 3; j++)
      {
      p->AddConstraint(new BinaryDifference(p, XCd2[j], new Square(p,Rc)), 0.0, 0.0);
      }

    /*

    // Add variables for cotan alpha
    for(int j = 0; j < 3; j++)
      {
      // Edge length opposite vertex j
      Expression *elen = p->AddVariable("len", sqrt(elen2[j]->Evaluate()), 0);

      // Make it equal its square!
      Expression *conEdge = new BinaryDifference(p, new Square(p, elen), elen2[j]);
      p->AddConstraint(conEdge, 0, 0);

      // The distance from circumcenter to edge
      Expression *height = p->AddVariable("hgt", sqrt(Rc->Evaluate()*Rc->Evaluate() - 0.25 * elen->Evaluate() * elen->Evaluate()), 0);

      // Simple linking expressions: 0.25 * elen^2 + h^2 = r^2

      Expression *conPytha = new TernarySum(p,
                                            new ScalarProduct(p, new Square(p, elen), 0.25),
                                            new Square(p, height),
                                            new Negation(p, new Square(p, Rc)));

      p->AddConstraint(conPytha, 0, 0);

      // The cotan we care about!
      Expression *cotA = p->AddVariable("cotA", height->Evaluate() / elen->Evaluate(), 0);

      // cot-alpha * elen = height
      p->AddConstraint(new BinaryDifference(p,
                                            new BinaryProduct(p, cotA, elen),
                                            height), 0, 0);

      printf("CotA[%d] : %f\n", j, cotA->Evaluate());
      }
      */

    }

#endif

#define ASPECTRATIO 1

#ifdef ASPECTRATIO_OLD

  // ------------------------------------------------------------------------
  // Add a constraint on triangle aspect ratios
  // ------------------------------------------------------------------------

  // To create this constraint, we need to first create a variable for the
  // length of every edge in the mesh.
  typedef std::pair<int, int> EdgeType;
  typedef std::map<EdgeType, Expression *> EdgeLenMap;
  EdgeLenMap edgeLenMap, edgeLenMapSqr;

  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {
    // Look across from each vertex in the triangle
    VarVec edge(3, NULL);
    VarVec edgesq(3, NULL);

    for(int j = 0; j < 3; j++)
      {
      int k1 = trit.GetBoundaryIndex((j + 1) % 3);
      int k2 = trit.GetBoundaryIndex((j + 2) % 3);
      EdgeType e = std::make_pair(std::min(k1, k2), std::max(k1, k2));

      EdgeLenMap::const_iterator eit = edgeLenMap.find(e);
      if(eit == edgeLenMap.end())
        {
        // Compute the squared length of the egde
        Expression *len2 = new TernaryGradientMagnitudeSqr(
              p,
              new BinaryDifference(p, X[k1][0],X[k2][0]),
              new BinaryDifference(p, X[k1][1],X[k2][1]),
              new BinaryDifference(p, X[k1][2],X[k2][2]));

        // Create the variable for this edge
        edge[j] = p->AddVariable("len", sqrt(len2->Evaluate()), 0);
        edgesq[j] = new Square(p, edge[j]);

        // Constrain the variable to the actual length
        p->AddConstraint(new BinaryDifference(p, len2, edgesq[j]), 0, 0);

        // Store the edge length variable
        edgeLenMap[e] = edge[j];
        edgeLenMapSqr[e] = edgesq[j];
        }
      else
        {
        edge[j] = eit->second;
        edgesq[j] = edgeLenMapSqr[e];
        }
      }

/*
    // Create a variable for the cosine of each angle in the triangle
    for(int j = 0; j < 3; j++)
      {
      int k0 = trit.GetBoundaryIndex(j);
      int k1 = trit.GetBoundaryIndex((j + 1) % 3);
      int k2 = trit.GetBoundaryIndex((j + 2) % 3);

      // cos(theta) = dot(u,v) / (|u| * |v|)
      Expression *UdotV = DotProduct(
            p,
            VectorApplyPairwise<BinaryDifference>(p, X[k1], X[k0]),
            VectorApplyPairwise<BinaryDifference>(p, X[k2], X[k0]));

      Expression *lenU = edge[(j+1) % 3];
      Expression *lenV = edge[(j+2) % 3];

      // Add the cosine of alpha with constraints on range
      Expression *cosAlpha = p->AddVariable(
            "cos", UdotV->Evaluate() / (lenU->Evaluate() * lenV->Evaluate()),
            0, acos(vnl_math::pi * 15 / 180));

      // Create a constraint linking the cosine
      Expression *constr = new BinaryDifference(
            p, new TernaryProduct(p, lenU, lenV, cosAlpha), UdotV);

      p->AddConstraint(constr, 0, 0);
      }
*/

    // Create constraints based on the cosine rule
    double min_angle = vnl_math::pi * 20 / 180;
    double max_cos = cos(min_angle);

    for(int j = 0; j < 3; j++)
      {
      Expression *c = edge[j], *a = edge[(j+1) % 3], *b = edge[(j+2) % 3];
      Expression *c2 = edgesq[j], *a2 = edgesq[(j+1) % 3], *b2 = edgesq[(j+2) % 3];

      // (c^2 - 2 * a * b * cosTheta) - (a^2 + b^2)
      Expression *constr = new BinaryDifference(
            p,
            new BinarySum(
              p, c2, new ScalarProduct(p, new BinaryProduct(p, a, b), 2 * max_cos)),
            new BinarySum(p, a2, b2));
      p->AddConstraint(constr, 0.0, ConstrainedNonLinearProblem::UBINF);
      }
    }

#endif

  // ------------------------------------------------------------------------
  // Add a constraint on minimal angle of boundary triangles
  // ------------------------------------------------------------------------
  double min_angle = vnl_math::pi * 12 / 180;
  double max_csc = 1.0 / sin(min_angle);

  for(int k = 0; k < bmesh->triangles.size(); k++)
    {
    for(int d = 0; d < 3; d++)
      {
      Expression *l1 = TEL_X[k][(d + 1) % 3];
      Expression *l2 = TEL_X[k][(d + 2) % 3];
      double v_csc_alpha = (l1->Evaluate() * l2->Evaluate()) / (2 * taX[k]->Evaluate());

      // Create a constrainted variable for the angle
      Expression *csc_alpha = p->AddVariable("cscAlpha", v_csc_alpha,
                                             ConstrainedNonLinearProblem::LBINF,
                                             max_csc);

      // Tie using the constraints, based on fmla 2*At*csc(alpha) = l1 * l2;
      Expression *con = new BinaryDifference(
            p,
            new ScalarProduct(p, new BinaryProduct(p, taX[k], csc_alpha), 2.0),
            new BinaryProduct(p, l1, l2));

      if(fabs(con->Evaluate()) > 1e-6)
        std::cout << "Con-CSC: " << con->Evaluate() << std::endl;

      p->AddConstraint(con, "CSC", 0, 0);
      }
    }

  /*
  double constEdgeRatio = 0.1;
  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {
    // Compute the squared edge length opposite each vertex
    Expression *edge[3];
    for(int j = 0; j < 3; j++)
      {
      int k1 = trit.GetBoundaryIndex((j + 1) % 3);
      int k2 = trit.GetBoundaryIndex((j + 2) % 3);
      edge[j] = new TernaryGradientMagnitudeSqr(p,
                                                new BinaryDifference(p, X[k1][0],X[k2][0]),
                                                new BinaryDifference(p, X[k1][1],X[k2][1]),
                                                new BinaryDifference(p, X[k1][2],X[k2][2]));
      }

    // Create the constraint for each edge
    for(int j = 0; j < 3; j++)
      {
      p->AddConstraint(
            new BinaryDifference(p, edge[j],
                                 new ScalarProduct(p, edge[(j+1) % 3], constEdgeRatio)),
            0.0, ConstrainedNonLinearProblem::UBINF);
      }
    }
  */

  // ------------------------------------------------------------------------
  // Derive an image match objective
  // ------------------------------------------------------------------------

#ifdef USE_DICE
  VarVecArray sampleX;
  VarVec sampleF;
  Expression *objObjectIntegral, *objVolumeIntegral;

  CreateOverlapObjective(p, model, &interp_new, X, M, U,
                         &objObjectIntegral, &objVolumeIntegral,
                         sampleX, sampleF);

  /*
  CreateAreaElementScaledOverlapObjective(p, model, &interp_new, X, M, R,
                                          AeltX, AeltM,
                                          &objObjectIntegral, &objVolumeIntegral,
                                          sampleX, sampleF);
  */


  /*
  VolumeOverlapEnergyTerm compterm(model, image, 3);
  SolutionData sd(model->GetIterationContext(), model->GetAtomArray());
  sd.ComputeIntegrationWeights();
  compterm.ComputeEnergy(&sd);

  printf("*** Overlap term report ***\n");
  printf("ObjInt = %f, VolInt = %f, Samples = %d\n",
         objObjectIntegral->Evaluate(),
         objVolumeIntegral->Evaluate(),
         sampleF.size());
  compterm.PrintReport(std::cout);
  */




  /*

  // Integrate match over wedges. The factor of 18 here accounts for the
  // fact that wedgeVol is scaled by six
  Expression *sampleScaleFactor = new Constant(
        p, model->GetNumberOfBoundaryPoints() * 1.0 / (18 * sampleF.size()));

  for(MedialBoundaryTriangleIterator trit = model->GetBoundaryTriangleIterator();
      !trit.IsAtEnd(); ++trit)
    {
    Expression *meanWedgeMatch = new BinaryProduct(
          p,
          sampleScaleFactor,
          new TernarySum(
            p,
            spokeMatch[trit.GetBoundaryIndex(0)],
            spokeMatch[trit.GetBoundaryIndex(1)],
            spokeMatch[trit.GetBoundaryIndex(2)]));

    objImageMatch->AddSummand(
          new BinaryProduct(p, meanWedgeMatch, wedgeVol[trit.GetIndex()]));
    }
    */


  // Compute a Dice-like objective value
  double volTarget = image->ComputeObjectVolume();
  Expression *objDice = new BinaryDifference(
        p, new Constant(p, 1.0),
        new ScalarProduct(p, objObjectIntegral, 1.0 / volTarget));

#endif // USE_DICE

  // This simple objective is the displacement from the input boundary points.
  // It allows us to resolve all the constraints without fitting to new data.
  // It's a good sanity check to make sure the model is valid
  BigSum *objDisplacement = new BigSum(p);
  for(int i = 0; i < nb; i++)
    {
    objDisplacement->AddSummand(
          new TernaryGradientMagnitudeSqr(
            p,
            new BinaryDifference(p, X[i][0], new Constant(p, X[i][0]->Evaluate())),
            new BinaryDifference(p, X[i][1], new Constant(p, X[i][1]->Evaluate())),
            new BinaryDifference(p, X[i][2], new Constant(p, X[i][2]->Evaluate()))));
    }

  // ------------------------------------------------------------------------
  // Derive the final objective
  // ------------------------------------------------------------------------

  // obj->AddSummand(objSqDist);
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, scaleRecip), objRecipSqDist));
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 20), objImageMatch));
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 0.01), objSurfArea));


  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 0.5), objSimpleBending));

  // Set of objectives for fitting the model to itself
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 1), objDisplacement));
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 0.1), objSimpleBending));
  // obj->AddSummand(new BinaryProduct(p, new Constant(p, 0.02), objSurfArea));

  // Dice!
  /*
  obj->AddSummand(new ScalarProduct(p, objDice, 4 * (nb + nm)));
  obj->AddSummand(new ScalarProduct(p, objBasisResidual, 1));
  */


  // ------------------------------------------------------------------------
  // Create diffeomorphic constraints for the mesh
  // ------------------------------------------------------------------------
  // CreateTetgenMesh(model, image, X, p);

  // ------------------------------------------------------------------------
  // Create a kernel for smoothing the gradient
  // ------------------------------------------------------------------------
  /*
  typedef ImmutableSparseMatrix<double> SparseMatrix;
  SparseMatrix::STLSourceType kSource(p->GetNumberOfVariables());

  double alpha = 0.2;
  for(MedialBoundaryPointIterator it = model->GetBoundaryPointIterator();
      !it.IsAtEnd(); ++it)
    {
    int iBnd = it.GetIndex();
    int iAtom = it.GetAtomIndex();

    for(int j = 0; j < 3; j++)
      {
      int idx_i = static_cast<Variable *>(X[iBnd][j])->GetIndex();
      for(EdgeWalkAroundVertex walk(mesh, iBnd); !walk.IsAtEnd(); ++walk)
        {
        int k = walk.MovingVertexId();
        int idx_j = static_cast<Variable *>(X[k][j])->GetIndex();
        SparseMatrix::STLEntryType entry;
        entry.first = idx_j;
        entry.second = alpha / walk.Valence();
        kSource[idx_i].push_back(entry);
        }
      kSource[idx_i].push_back(make_pair(idx_i, 1.0 - alpha));
      }
    }

  // Initialize the rest of the matrix to identity
  for(int i = 0; i < kSource.size(); i++)
    {
    if(kSource[i].size() == 0)
      kSource[i].push_back(make_pair(i, 1.0));
    }

  // Create a sparse matrix and multiply by itself a couple times
  SparseMatrix K;
  K.SetFromSTL(kSource, kSource.size());
  for(int q = 0; q < 2; q++)
    SparseMatrix::Multiply(K, K, K);
  p->SetGradientSmoothingKernel(K);
  */

  // Save the true objective
  // SaveGradient(p, model, X, obj, "grad_obj_before.vtk");



#ifdef USE_DICE
  std::cout << "Dice objective: " << 1- objDice->Evaluate() << std::endl;
  std::cout << "Image match: " << objObjectIntegral->Evaluate() << std::endl;
  std::cout << "Volume integral: " << objVolumeIntegral->Evaluate() << std::endl;
  std::cout << "Target volume: " << volTarget << std::endl;

  // Save the sample and image values at samples
  SaveSamples(sampleX, sampleF, "samples_before.vtk");

  // Plot the gradient of the problem
  SaveGradient(p, model, X, objDice, "grad_obj_dice_before.vtk");
  SaveGradient(p, model, X, objSimpleBending, "grad_obj_bend_before.vtk");
#endif


  // Test some derivatives;
  // DerivativeTest(p, 1000);

  // Solve the problem
  SmartPtr<IPOptProblemInterface> ip = new IPOptProblemInterface(p);

  // Set up the IPopt problem
  // Create a new instance of IpoptApplication
  //  (use a SmartPtr, not raw)
  // We are using the factory, since this allows us to compile this
  // example with an Ipopt Windows DLL
  SmartPtr<IpoptApplication> app = IpoptApplicationFactory();

  // Change some options
  // Note: The following choices are only examples, they might not be
  //       suitable for your optimization problem.
  app->Options()->SetNumericValue("tol", 1e-8);
  app->Options()->SetStringValue("linear_solver", "ma86");
  // app->Options()->SetNumericValue("mu_init", 1e-3);
  // app->Options()->SetNumericValue("mu_target", 1e-5);
  // app->Options()->SetStringValue("mu_strategy", "adaptive");
  // app->Options()->SetStringValue("output_file", "ipopt.out");

  app->Options()->SetIntegerValue("max_iter", 200);
  // app->Options()->SetStringValue("hessian_approximation", "limited-memory");
  // app->Options()->SetStringValue("derivative_test", "second-order");
  // app->Options()->SetStringValue("derivative_test_print_all", "yes");

  // Intialize the IpoptApplication and process the options
  ApplicationReturnStatus status;
  status = app->Initialize();
  if (status != Solve_Succeeded) {
    printf("\n\n*** Error during initialization!\n");
    return (int) status;
    }

  // Try just fitting the boundary data
  BigSum *obj;
  obj = new BigSum(p);
  obj->AddSummand(new ScalarProduct(p, objDisplacement, 1));
  obj->AddSummand(new ScalarProduct(p, objBasisResidual, 1));

  // Configure the problem
  p->SetObjective(obj);
  p->SetupProblem(true);

  // Evaluate the objective
  std::cout << "Displacement objective: " << objDisplacement->Evaluate() << std::endl;
  std::cout << "Residual objective: " << objBasisResidual->Evaluate() << std::endl;
  std::cout << "Surface area: " << objSurfArea->Evaluate() << std::endl;
  std::cout << "Model volume: " << objVolume->Evaluate() / 6 << std::endl;
  std::cout << "Total objective: " << obj->Evaluate() << std::endl;

  // Ask Ipopt to solve the problem
  status = app->OptimizeTNLP(GetRawPtr(ip));

  // Evaluate the objective
  std::cout << "Displacement objective: " << objDisplacement->Evaluate() << std::endl;
  std::cout << "Residual objective: " << objBasisResidual->Evaluate() << std::endl;
  std::cout << "Surface area: " << objSurfArea->Evaluate() << std::endl;
  std::cout << "Model volume: " << objVolume->Evaluate() / 6 << std::endl;
  std::cout << "Total objective: " << obj->Evaluate() << std::endl;

  // Save the current state
  SaveBoundaryMesh("fittoinput_bnd.vtk", p, bmesh, mIndex, mtbIndex, X, N, R);
  SaveMedialMesh("fittoinput_med.vtk", p, bmesh, mIndex, M, R);

  // Create the closest point finder
  ClosestPointMatcher cpmatcher(target, X.size());

  // Repeat this several times
  Expression *objSqDist, *objRecipSqDist;
  for(int i = 0; i < 5; i++)
    {
    // ------------------------------------------------------------------------
    // Construct the first part of the objective function
    // ------------------------------------------------------------------------
    objSqDist = ComputeDistanceToMeshObjective(p, &cpmatcher, X);

    // ------------------------------------------------------------------------
    // Construct an opposite objective: distance from a selected set of points
    // to the closest point on the medial mesh
    // ------------------------------------------------------------------------
    objRecipSqDist = ComputeDistanceToModelObjective(p, &cpmatcher, X);

    // ICP-style
    obj = new BigSum(p);
    obj->AddSummand(objSqDist);
    obj->AddSummand(new ScalarProduct(p, objRecipSqDist, 1));
    obj->AddSummand(new ScalarProduct(p, objBasisResidual, 1));

    // Configure the problem
    p->SetObjective(obj);
    p->SetupProblem(true);

    // Evaluate the objective
    std::cout << "MSD to target: " << objSqDist->Evaluate() << std::endl;
    std::cout << "MSD to model: " << objRecipSqDist->Evaluate() << std::endl;
    std::cout << "Surface area: " << objSurfArea->Evaluate() << std::endl;
    std::cout << "Model volume: " << objVolume->Evaluate() / 6 << std::endl;
    std::cout << "Displacement objective: " << objDisplacement->Evaluate() << std::endl;
    std::cout << "Residual objective: " << objBasisResidual->Evaluate() << std::endl;
    std::cout << "Total objective: " << obj->Evaluate() << std::endl;

    // Ask Ipopt to solve the problem
    status = app->OptimizeTNLP(GetRawPtr(ip));

    // Evaluate the objective
    std::cout << "MSD to target: " << objSqDist->Evaluate() << std::endl;
    std::cout << "MSD to model: " << objRecipSqDist->Evaluate() << std::endl;
    std::cout << "Surface area: " << objSurfArea->Evaluate() << std::endl;
    std::cout << "Model volume: " << objVolume->Evaluate() / 6 << std::endl;
    std::cout << "Displacement objective: " << objDisplacement->Evaluate() << std::endl;
    std::cout << "Residual objective: " << objBasisResidual->Evaluate() << std::endl;
    std::cout << "Total objective: " << obj->Evaluate() << std::endl;

    }

  std::cout << "MSD to target: " << objSqDist->Evaluate() << std::endl;
  std::cout << "MSD to model: " << objRecipSqDist->Evaluate() << std::endl;
  std::cout << "Surface area: " << objSurfArea->Evaluate() << std::endl;
  std::cout << "Model volume: " << objVolume->Evaluate() / 6 << std::endl;
  std::cout << "Displacement objective: " << objDisplacement->Evaluate() << std::endl;
  std::cout << "Residual objective: " << objBasisResidual->Evaluate() << std::endl;
  std::cout << "Total objective: " << obj->Evaluate() << std::endl;


#ifdef USE_DICE
  std::cout << "Image match: " << objObjectIntegral->Evaluate() << std::endl;
  std::cout << "Volume integral: " << objVolumeIntegral->Evaluate() << std::endl;
  std::cout << "Dice objective: " << 1- objDice->Evaluate() << std::endl;
  std::cout << "Target volume: " << volTarget << std::endl;
#endif

  if (status == Solve_Succeeded) {
    printf("\n\n*** The problem solved!\n");
    }
  else {
    printf("\n\n*** The problem FAILED!\n");
    }

  // Test some derivatives;
  // DerivativeTest(p, 1000);

  // Save the result as a boundary mesh
  SaveBoundaryMesh("result_bnd.vtk", p, bmesh, mIndex, mtbIndex, X, N, R);
  SaveMedialMesh("result_med.vtk", p, bmesh, mIndex, M, R);

  SaveGradient(p, X, obj, "grad_obj_after.vtk");

#ifdef USE_DICE
  // Save the sample and image values at samples
  SaveSamples(sampleX, sampleF, "samples_after.vtk");

  // Plot the gradient of the problem
  SaveGradient(p, X, objDice, "grad_obj_dice_after.vtk");
  SaveGradient(p, X, objBasisResidual, "grad_obj_residual_after.vtk");
#endif


#ifdef CIRCUMCENTER
  SaveCircumcenterMesh(CC, CR, CCBC);
#endif

  // As the SmartPtrs go out of scope, the reference count
  // will be decremented and the objects will automatically
  // be deleted.
  delete p;

  return (int) status;
}
