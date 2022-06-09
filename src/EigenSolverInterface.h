#ifndef __EigenSparseSolver_h_
#define __EigenSparseSolver_h_

#include <iostream>
#include "SparseSolver.h"
#include "SparseMatrix.h"

template <class TIndex>
class EigenSolverInterfaceInternal;

class EigenSolverInterface : public SparseSolver
{
public:

  enum ProblemType { SPD=0, UNSYMMETRIC };

  // Factor the system for arbitrary right hand sides and matrices of the same
  // non-zer element structure
  void SymbolicFactorization(size_t n, int *idxRows, int *idxCols, double *xMatrix);

  // Perform symbolic factorization given a matrix
  void SymbolicFactorization(const ImmutableSparseMatrix<double> &mat);

  // Factor the system for a specific matrix, but arbitrary right hand side
  void NumericFactorization(const double *xMatrix);

  // Numeric factorization using sparse matrix datatype
  void NumericFactorization(const ImmutableSparseMatrix<double> &mat)
    { NumericFactorization(mat.GetSparseData()); }

  // Solve the system for the given right hand side, solution in xSoln
  void Solve(double *xRhs, double *xSoln);

  // Solve the system for a number of right hand sides, if the second vector
  // is NULL, will solve in-place
  void Solve(size_t nRHS, double *xRhs, double *xSoln);

  // Outut dumping
  void SetVerbose(bool flag)
    { flagVerbose = flag; }

  // Constructor, takes the problem type
  EigenSolverInterface(ProblemType ptype);

  // Destructor, get rid of matrix
  virtual ~EigenSolverInterface();

protected:

  EigenSolverInterfaceInternal<int> *m_InternalSolver;

  ProblemType m_Type;

  // Reset the index arrays()
  void ResetIndices();

  // Copy of the passed in arrays
  int *m_RowIndex = nullptr, *m_ColIndex = nullptr;
};

#endif //__EigenSparseSolver_h_
