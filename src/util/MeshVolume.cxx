#include <iostream>
#include <string>

#include <vnl/vnl_inverse.h>

#include "ReadWriteVTK.h"
#include "vtkMassProperties.h"
#include "vtkDataReader.h"
#include "vtkUnstructuredGrid.h"

using namespace std;

int vtkmeshvol_backwards_usage()
{
  cout << "vtkmeshvol - Compute the volume of a triangular mesh" << endl;
  cout << "usage: " << endl;
  cout << "   vtkmeshvol mesh.vtk" << endl;
  return -1;
}

template <class TMeshType> int ComputeMeshVolume(const char *fn_mesh)
{
  return 0;
}

template <> int ComputeMeshVolume<vtkPolyData>(const char *fn_mesh)
{
  // Read the mesh
  vtkSmartPointer<vtkPolyData> mesh = ReadMesh<vtkPolyData>(fn_mesh);
  vtkNew<vtkMassProperties> mp;
  mp->SetInputData(mesh);
  mp->Update();
  std::cout << "Mesh volume = " << mp->GetVolume() << std::endl;
  return 0;
}


  

int main(int argc, char **argv)
{
  // Check the parameters
  if(argc < 2) return vtkmeshvol_backwards_usage();
  const char *fn_mesh = argv[1];

  // Check the data type of the input file
  vtkNew<vtkDataReader> reader;
  reader->SetFileName(fn_mesh);
  reader->OpenVTKFile();
  reader->ReadHeader();

  // Is this a polydata?
  if(reader->IsFileUnstructuredGrid())
    {
    return ComputeMeshVolume<vtkUnstructuredGrid>(fn_mesh);
    }
  else if(reader->IsFilePolyData())
    {
    return ComputeMeshVolume<vtkPolyData>(fn_mesh);
    }
  else
    {
    cerr << "Unsupported VTK data type in input file" << endl;
    return -1;
    }
}
