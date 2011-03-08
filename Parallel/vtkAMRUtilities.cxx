/*=========================================================================

 Program:   Visualization Toolkit
 Module:    vtkAMRUtilities.cxx

 Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
 All rights reserved.
 See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notice for more information.

 =========================================================================*/
#include "vtkAMRUtilities.h"
#include "vtkAMRBox.h"
#include "vtkUniformGrid.h"
#include "vtkHierarchicalBoxDataSet.h"
#include "vtkMultiProcessController.h"
#include "vtkMPIController.h"
#include "vtkCommunicator.h"

#include <cmath>
#include <limits>
#include <cassert>

//------------------------------------------------------------------------------
void vtkAMRUtilities::PrintSelf( std::ostream& os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
}

//------------------------------------------------------------------------------
void vtkAMRUtilities::ComputeDataSetOrigin(
       double origin[3], vtkHierarchicalBoxDataSet *amrData,
       vtkMultiProcessController *controller )
{
  // Sanity check
  assert( "Input AMR Data is NULL" && (amrData != NULL) );

  double min[3];
  min[0] = min[1] = min[2] = 100;

  // Note, we only need to check at level 0 since, the grids at
  // level 0 are guaranteed to cover the entire domain. Most datasets
  // will have a single grid at level 0.
  for( int idx=0; idx < amrData->GetNumberOfDataSets(0); ++idx )
    {

      vtkUniformGrid *gridPtr = amrData->GetDataSet( 0, idx );
      if( gridPtr != NULL )
        {
          double *gridBounds = gridPtr->GetBounds();
          assert( "Failed when accessing grid bounds!" && (gridBounds!=NULL) );

          if( gridBounds[0] < min[0] )
            min[0] = gridBounds[0];
          if( gridBounds[2] < min[1] )
            min[1] = gridBounds[2];
          if( gridBounds[4] < min[2] )
            min[2] = gridBounds[4];

        }

    } // END for all data-sets at level 0

  // If data is distributed, get the global min
  if( controller != NULL )
    {
      if( controller->GetNumberOfProcesses() > 1 )
        {
          // TODO: Define a custom operator s.t. only one all-reduce operation
          // is called.
          controller->AllReduce(&min[0],&origin[0],1,vtkCommunicator::MIN_OP);
          controller->AllReduce(&min[1],&origin[1],1,vtkCommunicator::MIN_OP);
          controller->AllReduce(&min[2],&origin[2],1,vtkCommunicator::MIN_OP);
          return;
        }
    }

   // Else this is a single process
   origin[0] = min[0];
   origin[1] = min[1];
   origin[2] = min[2];
}

//------------------------------------------------------------------------------
void vtkAMRUtilities::CollectAMRMetaData(
    vtkHierarchicalBoxDataSet *amrData,
    vtkMultiProcessController *myController )
{
  // Sanity check
  assert( "Input AMR Data is NULL" && (amrData != NULL));

  // STEP 0: Compute the global dataset origin
  double origin[3];
  ComputeDataSetOrigin( origin, amrData, myController );

  // STEP 1: Compute the metadata of each process locally
  int process = (myController == NULL)? 0 : myController->GetLocalProcessId();
  ComputeLocalMetaData( origin, amrData, process );

  // STEP 2: Distribute meta-data to all processes
  if( myController != NULL )
    {
      DistributeMetaData( amrData, myController );
    }

}

//------------------------------------------------------------------------------
void vtkAMRUtilities::SerializeMetaData(
    vtkHierarchicalBoxDataSet *amrData,
    unsigned char *&buffer,
    vtkIdType &numBytes )
{
  // Sanity check
  assert( "Input AMR Data is NULL" && (amrData != NULL) );

  // STEP 0: Collect all the AMR boxes in a vector
  std::vector< vtkAMRBox > boxList;
  for( int level=0; level < amrData->GetNumberOfLevels(); ++level )
    {
      for( int idx=0; idx < amrData->GetNumberOfDataSets( level ); ++idx )
        {

          if( amrData->GetDataSet(level,idx) != NULL )
            {
              vtkAMRBox myBox;
              amrData->GetMetaData(level,idx,myBox);
              boxList.push_back( myBox );
            }

        } // END for all data at the current level
    } // END for all levels

  // STEP 1: Compute & Allocate buffer size
  int N    = boxList.size( );
  numBytes = sizeof( int ) + vtkAMRBox::GetBytesize()*N;
  buffer   = new unsigned char[ numBytes ];

  // STEP 2: Serialize the number of boxes in the buffer
  unsigned char *ptr = buffer;
  memcpy( ptr, &N, sizeof(int) );
  ptr += sizeof(int);

  // STEP 3: Serialize each box
  for( int i=0; i < boxList.size( ); ++i )
    {
      assert( "ptr is NULL" && (ptr != NULL) );

      unsigned char *tmp = NULL;
      vtkIdType nbytes      = 0;
      boxList[ i ].Serialize( tmp, nbytes );
      memcpy( ptr, tmp, vtkAMRBox::GetBytesize() );
      ptr += vtkAMRBox::GetBytesize();
    }

}

//------------------------------------------------------------------------------
void vtkAMRUtilities::DeserializeMetaData(
    unsigned char *buffer,
    const vtkIdType numBytes,
    std::vector< vtkAMRBox > &boxList )
{
  // Sanity check
  assert( "Buffer to deserialize is NULL" && (buffer != NULL) );
  assert( "Expected numBytes > 0" && (numBytes > 0) );

  unsigned char *ptr = buffer;
  int N              = 0;

  // STEP 0: Deserialize the number of boxes in the buffer
  memcpy( &N, ptr, sizeof(int) );
  ptr += sizeof(int);

  boxList.resize( N );
  for( int i=0; i < N; ++i )
    {
      assert( "ptr is NULL" && (ptr != NULL) );

      vtkIdType nbytes = vtkAMRBox::GetBytesize();
      boxList[ i ].Deserialize( ptr, nbytes );
      ptr += nbytes;
    }

}

//------------------------------------------------------------------------------
void vtkAMRUtilities::DistributeMetaData(
    vtkHierarchicalBoxDataSet *amrData,
    vtkMultiProcessController *myController )
{
  // Sanity check
  assert( "Input AMR Data is NULL" && (amrData != NULL) );
  assert( "Multi-Process controller is NULL" && (myController != NULL) );

  // STEP 0: Serialize the meta-data owned by this process into a bytestream
  unsigned char *buffer = NULL;
  vtkIdType numBytes       = 0;
  SerializeMetaData( amrData, buffer, numBytes );
  assert( "Serialized buffer should not be NULL!" && (buffer != NULL) );
  assert( "Expected NumBytes > 0" && (numBytes > 0) );

  // STEP 1: Get the buffer sizes at each rank with an allGather
  int numRanks   = myController->GetNumberOfProcesses();
  vtkIdType *rcvcounts = new vtkIdType[ numRanks ];
  myController->AllGather( &numBytes, rcvcounts, 1);

  // STEP 2: Compute the receive buffer & Allocate
  vtkIdType rbufferSize = rcvcounts[0];
  for( int i=1; i < numRanks; ++i)
    rbufferSize+=rcvcounts[i];
  unsigned char *rcvBuffer = new unsigned char[ rbufferSize ];
  assert( "Receive buffer is NULL" && (rcvBuffer != NULL) );

  // STEP 3: Compute off-sets
  vtkIdType *offSet = new vtkIdType[ numRanks];
  offSet[0] = 0;
  for( int i=1; i < numRanks; ++i )
    offSet[ i ] = offSet[ i-1 ]+rcvcounts[ i-1 ];

  // STEP 4: All-gatherv boxes
  myController->AllGatherV( buffer, rcvBuffer, numBytes, rcvcounts, offSet );

  // STEP 5: Unpack receive buffer
  std::vector< std::vector< vtkAMRBox > > amrBoxes;
  amrBoxes.resize( numRanks );
  for( int i=0; i < numRanks; ++i )
    {
      DeserializeMetaData( rcvBuffer+offSet[i],rcvcounts[i],amrBoxes[i] );
    }

  // STEP 6: Clean up all dynamicall allocated memory
  delete [] buffer;
  delete [] rcvcounts;
  delete [] offSet;
  delete [] rcvBuffer;

}

//------------------------------------------------------------------------------
void vtkAMRUtilities::CreateAMRBoxForGrid(
    double origin[3], vtkUniformGrid *myGrid, vtkAMRBox &myBox )
{
  // Sanity check
  assert( "Input AMR Grid is not NULL" && (myGrid != NULL) );

  double *gridOrigin = myGrid->GetOrigin();
  assert( "Null Grid Origin" && (gridOrigin != NULL)  );

  int ndim[3];
  int lo[3];
  int hi[3];

  // Get pointer to the grid's spacing array
  double *h = myGrid->GetSpacing();
  assert( "Grid Spacing array is NULL!" && (h!=NULL) );

  // Get the grid's cell dimensions,i.e., number of cells along each dimension.
  myGrid->GetDimensions( ndim );
  ndim[0]--; ndim[1]--; ndim[2]--;

  // Compute lo,hi box dimensions
  for( int i=0; i < 3; ++i )
    {
      ndim[i] = (ndim[i] < 1)? 1 : ndim[i];
      lo[i]   = round( (gridOrigin[i]-origin[i])/h[i] );
      hi[i]   = round( lo[i] + ( ndim[i]-1 ) );
    }

  myBox.SetDimensions( lo, hi );
  myBox.SetDataSetOrigin( origin );
  myBox.SetGridSpacing( h );
}

//------------------------------------------------------------------------------
void vtkAMRUtilities::ComputeLocalMetaData(
    double origin[3], vtkHierarchicalBoxDataSet* myAMRData, const int process )
{
  // Sanity check
  assert( "Input AMR data is NULL" && (myAMRData != NULL) );

  for( int level=0; level < myAMRData->GetNumberOfLevels(); ++level )
    {
      for( int idx=0; idx < myAMRData->GetNumberOfDataSets(level); ++idx )
        {

          vtkUniformGrid *myGrid = myAMRData->GetDataSet( level, idx );
          if( myGrid != NULL )
            {
              vtkAMRBox myBox;
              CreateAMRBoxForGrid( origin, myGrid, myBox );
              myBox.SetLevel( level );
              myBox.SetProcessId( process );
              myAMRData->SetMetaData( level, idx, myBox );

              // Write box for debugging purposes.
//              myBox.WriteBox();
            }

        } // END for all data at current level
    } // END for all levels
}

//------------------------------------------------------------------------------
void vtkAMRUtilities::ComputeLevelRefinementRatio(
    vtkHierarchicalBoxDataSet *amr )
{
  // sanity check
  assert( "Input AMR Data is NULL" && (amr != NULL)  );

  int numLevels = amr->GetNumberOfLevels();

  if( numLevels < 1 )
    {
      // Dataset is empty!
      return;
    }

  if( numLevels == 1)
    {
      // No refinement, data-set has only a single level.
      // The refinement ratio is set to 2 to satisfy the
      // vtkHierarchicalBoxDataSet requirement.
      amr->SetRefinementRatio(0,2);
      return;
    }

   for( int level=1; level < numLevels; ++level )
     {

       int parentLevel = level-1;
       assert("No data at parent!" && amr->GetNumberOfDataSets(parentLevel)>=1);
       assert("No data in this level" && amr->GetNumberOfDataSets(level)>=1 );

       vtkAMRBox parentBox;
       amr->GetMetaData(parentLevel,0,parentBox);

       vtkAMRBox myBox;
       amr->GetMetaData(level,0,myBox);

       double parentSpacing[3];
       parentBox.GetGridSpacing(parentSpacing);

       double currentSpacing[3];
       myBox.GetGridSpacing( currentSpacing );

       // Note current implementation assumes uniform spacing. The
       // refinement ratio is the same in each dimension i,j,k.
       int ratio = round(parentSpacing[0]/currentSpacing[0]);

       // Set the ratio at the root level, i.e., level 0, to be the same as
       // the ratio at level 1,since level 0 doesn't really have a refinement
       // ratio.
       if( level==1 )
         {
           amr->SetRefinementRatio(0,ratio);
         }
       amr->SetRefinementRatio(level,ratio);

     } // END for all hi-res levels

}
