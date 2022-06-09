#include "VCGTriMesh.h"
#include <vtkPolyData.h>
#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vcg/complex/algorithms/clean.h>


using namespace vcg;
using namespace std;

void VCGTriMesh::ImportFromVTK(vtkPolyData *pd)
{
  int np = pd->GetNumberOfPoints();

  // Remove existing elements
  m_Mesh.Clear();

  // Create adder functions
  Mesh::VertexIterator vi = vcg::tri::Allocator<Mesh>::AddVertices(m_Mesh, np);
  Mesh::FaceIterator fi = vcg::tri::Allocator<Mesh>::AddFaces(m_Mesh, pd->GetPolys()->GetNumberOfCells());

  // Container for created vertex pointers
  vector<Mesh::VertexPointer> ivp(np);
  for(unsigned int i = 0; i < np; i++)
    {
    ivp[i] = &*vi;
    auto *p = pd->GetPoint(i);
    vi->P() = Mesh::CoordType(p[0], p[1], p[2]);
    ++vi;
    }

  // Add the faces
  int bad_faces = 0;
  auto iter = vtk::TakeSmartPointer(pd->GetPolys()->NewIterator());
  for (iter->GoToFirstCell(); !iter->IsDoneWithTraversal(); iter->GoToNextCell())
  {
    // do work with iter
    const vtkIdType *ids;
    vtkIdType npts;
    iter->GetCurrentCell(npts, ids);
    if(npts != 3)
      {
      ++bad_faces;
      continue;
      }
    fi->V(0)=ivp[ids[0]];
    fi->V(1)=ivp[ids[1]];
    fi->V(2)=ivp[ids[2]];
    fi++;
  }

  if(bad_faces > 0)
    std::cerr << "Conversion from VTK to VCG encountered " << bad_faces << " non-triangular faces" << std::endl;
  }

#include <vtkPointData.h>
#include <vtkFloatArray.h>

void VCGTriMesh::ExportToVTK(vtkPolyData *pd)
{
  // Point array
  vtkNew<vtkPoints> pts;

  // Normal array
  vtkNew<vtkFloatArray> normals;
  normals->SetNumberOfComponents(3);

  std::vector<int> VertexId(m_Mesh.vert.size());
  int numvert = 0;
  for(auto vi = m_Mesh.vert.begin(); vi != m_Mesh.vert.end(); ++vi)
    {
    if( !(*vi).IsD() )
      {
      const auto &p = vi->cP();
      const auto &n = vi->cN();
      pts->InsertNextPoint(p[0], p[1], p[2]);
      normals->InsertNextTuple3(n[0], n[1], n[2]);

      VertexId[vi - m_Mesh.vert.begin()] = numvert++;
      }
    }

  pd->SetPoints(pts);
  pd->GetPointData()->SetNormals(normals);

  // Face array
  vtkNew<vtkCellArray> faces;
  for(auto fi = m_Mesh.face.begin(); fi != m_Mesh.face.end(); ++fi)
    {
    if( !(*fi).IsD() )
      {
      faces->InsertNextCell(fi->VN());
      for(int k=0; k< fi->VN(); k++)
        faces->InsertCellPoint(VertexId[tri::Index(m_Mesh, fi->V(k))]);
      }
    }

  pd->SetPolys(faces);
}

void VCGTriMesh::CleanMesh()
{
  // some cleaning to get rid of bad file formats like stl that duplicate vertexes..
  int dup = tri::Clean<Mesh>::RemoveDuplicateVertex(m_Mesh);
  int unref = tri::Clean<Mesh>::RemoveUnreferencedVertex(m_Mesh);
  printf("Removed %i duplicate and %i unreferenced vertices from mesh\n", dup, unref);

  tri::Allocator<Mesh>::CompactEveryVector(m_Mesh);
  tri::UpdateTopology<Mesh>::VertexFace(m_Mesh);
  tri::UpdateBounding<Mesh>::Box(m_Mesh);
  tri::UpdateFlags<Mesh>::FaceBorderFromVF(m_Mesh);
  tri::UpdateFlags<Mesh>::VertexBorderFromFaceBorder(m_Mesh);
}
