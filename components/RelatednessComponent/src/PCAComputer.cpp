
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/function.hpp>
#include "config/config.hpp"
#include "Eigen/Core"
#include "Eigen/Eigenvalues"
#include "genfile/VariantIdentifyingData.hpp"
#include "genfile/VariantDataReader.hpp"
#include "genfile/SNPDataSourceProcessor.hpp"
#include "genfile/SingleSNPGenotypeProbabilities.hpp"
#include "statfile/BuiltInTypeStatSink.hpp"
#include "statfile/BuiltInTypeStatSource.hpp"
#include "appcontext/FileUtil.hpp"
#include "appcontext/get_current_time_as_string.hpp"
#include "appcontext/OptionProcessor.hpp"
#include "appcontext/UIContext.hpp"
#include "components/SampleSummaryComponent/SampleStorage.hpp"
#include "components/RelatednessComponent/PCAComputer.hpp"
#include "components/RelatednessComponent/LapackEigenDecomposition.hpp"
#include "components/RelatednessComponent/names.hpp"
#include "components/RelatednessComponent/write_matrix_to_stream.hpp"

PCAComputer::PCAComputer(
	appcontext::OptionProcessor const& options,
	genfile::CohortIndividualSource const& samples,
	appcontext::UIContext& ui_context
):
	m_options( options ),
	m_ui_context( ui_context ),
	m_samples( samples ),
	m_number_of_samples( samples.get_number_of_individuals() ),
	m_number_of_PCs_to_compute( std::min( m_options.get< std::size_t >( "-PCs" ), samples.get_number_of_individuals() ) ),
	m_threshhold( 0.9 )
{}

void PCAComputer::compute( Eigen::MatrixXd const& matrix, std::size_t const number_of_snps, std::string const& name ) {
	m_number_of_snps = number_of_snps ;
	m_kinship_matrix = matrix ;
	m_filename = name ;

	std::cerr << "PCAComputer::set_relatedness_matrix(): set relatedness matrix to:\n"
		<< m_kinship_matrix.block( 0, 0, 10, 10) << ".\n" ;

	compute_PCA() ;
}

namespace {
	genfile::VariantEntry get_eigendecomposition_header( std::string const& prefix, int value ) {
		if( value == 0 ) {
			return genfile::VariantEntry( "eigenvalue" ) ;
		}
		else {
			return genfile::VariantEntry( prefix + genfile::string_utils::to_string( value ) ) ;
		}
	}
}

void PCAComputer::load_matrix(
	genfile::CohortIndividualSource const& samples,
	std::string const& filename,
	Eigen::MatrixXd* matrix,
	std::size_t* number_of_snps,
	appcontext::UIContext& ui_context
) {
	statfile::BuiltInTypeStatSource::UniquePtr source = statfile::BuiltInTypeStatSource::open( filename ) ;
	std::size_t number_of_samples = 0 ;
	load_matrix_metadata( samples, *source, &number_of_samples, number_of_snps, ui_context ) ;

	// Read the matrix, making sure the samples come in the same order as in the sample file.
	matrix->resize( samples.get_number_of_individuals(), samples.get_number_of_individuals() ) ;
	std::vector< std::size_t > sample_column_indices( samples.get_number_of_individuals() ) ;
	for( std::size_t sample_i = 0; sample_i < samples.get_number_of_individuals(); ++sample_i ) {
		sample_column_indices[sample_i] = source->index_of_column( pca::get_concatenated_sample_ids( &samples, sample_i )) ;
		if( sample_i > 0 ) {
			if( sample_column_indices[sample_i] <= sample_column_indices[sample_i-1] ) {
				ui_context.logger() << "!! Error in PCAComputer::load_matrix(): column order does not match samples.\n" ;
				throw genfile::MalformedInputError( source->get_source_spec(), 0, sample_column_indices[ sample_i ] ) ;
			}
		}
	}

	// Now apply sample exclusions.
	for( std::size_t sample_i = 0; sample_i < samples.get_number_of_individuals(); ++sample_i ) {
		std::string id ;
		// find row corresponding to next sample.
		for( (*source) >> id; (*source) && id != pca::get_concatenated_sample_ids( &samples, sample_i ); (*source) >> statfile::ignore_all() >> id ) ;
		if( !(*source) || source->number_of_rows_read() == source->number_of_rows() ) {
			throw genfile::MalformedInputError( source->get_source_spec(), source->number_of_rows_read(), 0 ) ;
		}
		if( source->number_of_rows_read() != ( sample_column_indices[ sample_i ] - 1 ) ) {
			ui_context.logger()
				<< "!! Error( PCAComputer::load_matrix ): sample " << samples.get_entry( sample_i, "id_1" )
				<< " is on row "
				<< source->number_of_rows_read()
				<< " but column "
				<< sample_column_indices[ sample_i ]
				<< ".\n" ;
			throw genfile::MalformedInputError( source->get_source_spec(), source->number_of_rows_read(), 0 ) ;
		}
		for( std::size_t sample_j = 0; sample_j < samples.get_number_of_individuals(); ++sample_j ) {
			(*source)
				>> statfile::ignore( sample_column_indices[ sample_j ] - source->current_column() )
				>> (*matrix)( sample_i, sample_j )
			;
		}
		(*source) >> statfile::ignore_all() ;
	}
}

void PCAComputer::load_long_form_matrix(
	genfile::CohortIndividualSource const& samples,
	std::string const& filename,
	Eigen::MatrixXd* matrix,
	std::size_t* number_of_snps,
	appcontext::UIContext& ui_context
) {
	statfile::BuiltInTypeStatSource::UniquePtr source = statfile::BuiltInTypeStatSource::open( filename ) ;
	std::size_t number_of_samples = 0 ;
	load_matrix_metadata( samples, *source, &number_of_samples, number_of_snps, ui_context ) ;

	matrix->resize( samples.get_number_of_individuals(), samples.get_number_of_individuals() ) ;
	Eigen::MatrixXd non_missingness( matrix->rows(), matrix->cols() ) ;
	non_missingness.setZero() ;
	
	std::map< std::string, int > samples_to_indices ;
	std::map< std::string, int > samples_filled ;
	for( std::size_t sample_i = 0; sample_i < samples.get_number_of_individuals(); ++sample_i ) {
		std::string const sample = samples.get_entry( sample_i, "ID_1" ).as< std::string >() ;
		samples_to_indices[ sample ] = sample_i ;
		samples_filled[ sample ] = 0 ;
	}

	{
		std::string sample1, sample2 ;
		int pairwise_complete_obs = 0 ;
		double value = 0 ;
		// Read lower diagonal
		while( (*source) >> sample1 >> sample2 >> pairwise_complete_obs >> value ) {
			int const i = samples_to_indices[ sample1 ] ;
			int const j = samples_to_indices[ sample2 ] ;
			if( non_missingness( i, j ) == 1 ) {
				throw genfile::MalformedInputError( source->get_source_spec(), source->number_of_rows_read() ) ;
			} else {
				(*matrix)( i, j ) = value ;
				non_missingness( i, j ) = 1 ;
			}
			(*source) >> statfile::ignore_all() ;
		}
		
		// Fill in upper diagonal
		for( int i = 0; i < matrix->rows(); ++i ) {
			for( int j = i+1; j < matrix->cols(); ++j ) {
				(*matrix)( i, j ) = (*matrix)( j, i ) ;
				non_missingness( i, j ) = non_missingness( j, i ) ;
			}
		}

#if DEBUG_PCA_COMPUTER		
		ui_context.logger() << "Matrix is:\n"
			<< (*matrix).block( 0, 0, 10, 10 )
			<< "\n" ;

		ui_context.logger() << "Non-missingness is:\n"
			<< non_missingness.block( 0, 0, 10, 10 )
			<< "\n" ;
#endif

		if( non_missingness.array().minCoeff() == 0 ) {
			int count = 0 ;
			for( int i = 0; i < matrix->rows() && count < 20; ++i ) {
				for( int j = 0; j < matrix->cols() && count < 20; ++j ) {
					if( non_missingness( i, j ) != 1 ) {
						ui_context.logger() << "PCAComputer::load_long_form_matrix(): sample pair "
							<< samples.get_entry( i, "ID_1" )
							<< " : "
							<< samples.get_entry( j, "ID_1" )
							<< " seems unrepresented in \""
							<< filename
							<< "\".\n" ;
						++count ;
					}
				}
			}
			throw genfile::BadArgumentError( "PCAComputer::load_long_form_matrix()", "filename=\"" + filename + "\"", "not every sample was represented in the matrix." ) ;
		}
	}
}

void PCAComputer::load_matrix_metadata(
	genfile::CohortIndividualSource const& samples,
	statfile::BuiltInTypeStatSource& source,
	std::size_t* number_of_samples,
	std::size_t* number_of_snps,
	appcontext::UIContext& ui_context
) {
	using namespace genfile::string_utils ;
	// Read the metadata
	std::size_t number_of_samples_ = 0, number_of_snps_ = 0 ;
	std::vector< std::string > metadata = split( source.get_descriptive_text(), "\n" ) ;
	for( std::size_t i = 0; i < metadata.size(); ++i ) {
		std::vector< std::string > elts = split_and_strip( metadata[i], ":", " " ) ;
		if( elts.size() == 2 && elts[0] == "Number of SNPs" ) {
			number_of_snps_ = to_repr< std::size_t >( elts[1] ) ;
		}
		else if( elts.size() == 2 && elts[0] == "Number of samples" ) {
			number_of_samples_ = to_repr< std::size_t >( elts[1] ) ;
		}
	}
	if( number_of_samples_ != samples.get_number_of_individuals() ) {
		throw genfile::MismatchError(
			"PCAComputer::load_matrix_metadata()",
			source.get_source_spec(),
			"number of samples: " + to_string( number_of_samples_ ),
			"expected number: " + to_string( samples.get_number_of_individuals() )
		) ;
	}
	*number_of_samples = number_of_samples_ ;
	*number_of_snps = number_of_snps_ ;
}

genfile::VariantEntry PCAComputer::get_pca_name( std::size_t i ) {
	return std::string( "PC_" ) + genfile::string_utils::to_string( i + 1 ) ;
}

void PCAComputer::compute_PCA() {
	Eigen::MatrixXd kinship_eigendecomposition( m_number_of_samples, m_number_of_samples + 1 ) ;
#if HAVE_LAPACK
	if( m_options.check( "-use-eigen" ))
#endif
	{
		m_ui_context.logger() << "========================================================================\n" ;
		m_ui_context.logger() << "PCAComputer: Computing eigenvalue decomposition of " << m_number_of_samples << "x" << m_number_of_samples << " kinship matrix using Eigen...\n" ;
		Eigen::SelfAdjointEigenSolver< Eigen::MatrixXd > solver( m_kinship_matrix ) ;
		if( solver.info() == Eigen::Success ) {
			m_ui_context.logger() << "PCAComputer: Done, writing results...\n" ;
		} else if( solver.info() == Eigen::NumericalIssue ) {
			m_ui_context.logger() << "PCAComputer: Oh dear, numerical issue, writing results...\n" ;
		} else if( solver.info() == Eigen::NoConvergence ) {
			m_ui_context.logger() << "PCAComputer: Oh dear, no convergence, writing results...\n" ;
		} else {
			m_ui_context.logger() << "PCAComputer: Oh dear, unknown error, writing results...\n" ;
		}
		kinship_eigendecomposition.block( 0, 0, m_number_of_samples, 1 ) = solver.eigenvalues().reverse() ;
		kinship_eigendecomposition.block( 0, 1, m_number_of_samples, m_number_of_samples ) = Eigen::Reverse< Eigen::MatrixXd, Eigen::Horizontal >( solver.eigenvectors() ) ;
	}
#if HAVE_LAPACK
	else { // -use-eigen not specified.
		Eigen::VectorXd eigenvalues( m_number_of_samples ) ;
		Eigen::MatrixXd eigenvectors( m_number_of_samples, m_number_of_samples ) ;
		m_ui_context.logger() << "PCAComputer: Computing eigenvalue decomposition of kinship matrix using lapack...\n" ;
		lapack::compute_eigendecomposition( m_kinship_matrix, &eigenvalues, &eigenvectors ) ;
		//lapack::compute_partial_eigendecomposition( m_kinship_matrix, &eigenvalues, &eigenvectors, m_number_of_PCs_to_compute ) ;
		kinship_eigendecomposition.block( 0, 0, m_number_of_samples, 1 ) = eigenvalues.reverse() ;
		kinship_eigendecomposition.block( 0, 1, m_number_of_samples, m_number_of_samples ) = Eigen::Reverse< Eigen::MatrixXd, Eigen::Horizontal >( eigenvectors ) ;
	}
#endif

	// Verify the decomposition.
	{
		std::size_t size = std::min( std::size_t(10), m_number_of_samples ) ;
		m_ui_context.logger() << "Top-left of U U^t is:\n" ;
		{
			
			Eigen::MatrixXd UUT = kinship_eigendecomposition.block( 0, 1, size, m_number_of_samples ) ;
			UUT *= kinship_eigendecomposition.block( 0, 1, m_number_of_samples, size ).transpose() ;
			pca::write_matrix_to_stream( m_ui_context.logger(), UUT ) ;
		}
		m_ui_context.logger() << "Top-left of original kinship matrix is:\n" ;
		pca::write_matrix_to_stream( m_ui_context.logger(), m_kinship_matrix.block( 0, 0, size, size )) ;
		
		Eigen::VectorXd v = kinship_eigendecomposition.block( 0, 0, m_number_of_samples, 1 ) ;
		Eigen::MatrixXd reconstructed_kinship_matrix = kinship_eigendecomposition.block( 0, 1, size, m_number_of_samples ) ;
		reconstructed_kinship_matrix *= v.asDiagonal() ;
		reconstructed_kinship_matrix *= kinship_eigendecomposition.block( 0, 1, m_number_of_samples, size ).transpose() ;
		// Knock out the upper triangle for comparison with the original kinship matrix.
		for( std::size_t i = 0; i < size; ++i ) {
			for( std::size_t j = i+1; j < size; ++j ) {
				reconstructed_kinship_matrix(i,j) = std::numeric_limits< double >::quiet_NaN() ;
			}
		}

		m_ui_context.logger() << "Top-left of reconstructed kinship matrix is:\n" ;
		pca::write_matrix_to_stream( m_ui_context.logger(), reconstructed_kinship_matrix.block( 0, 0, size, size )) ;

		m_ui_context.logger() << "Verifying the decomposition...\n" ;
		double diff = 0.0 ;
		for( std::size_t i = 0; i < size; ++i ) {
			for( std::size_t j = 0; j <= i; ++j ) {
				diff = std::max( diff, std::abs( reconstructed_kinship_matrix(i,j) - m_kinship_matrix( i, j ))) ;
			}
		}
		m_ui_context.logger() << "...maximum discrepancy in lower diagonal is " << diff << ".\n" ;
		if( diff != diff ) {
			m_ui_context.logger() << "...yikes, there were NaNs.\n" ;
		} else if( diff > 0.01 ) {
			m_ui_context.logger() << "...warning: diff > 0.01.\n" ;
		}
	}
	
	using genfile::string_utils::to_string ;

	send_UDUT(
		"Number of SNPs: " + to_string( m_number_of_snps ) + "\n" +
			"Number of samples: " + to_string( m_number_of_samples ) + "\n"
			"Note: this file contains an eigenvalue decomposition of the matrix in the file \"" + m_filename + "\"\n" +
			"The first column contains the eigenvalues of the decomposition, and subsequent columns contain the eigenvectors.",
		m_number_of_snps,
		kinship_eigendecomposition,
		boost::function< genfile::VariantEntry ( int ) >(),
		boost::bind(
			&get_eigendecomposition_header,
			"v",
			_1
		)
	) ;

	if( m_number_of_PCs_to_compute > 0 ) {
		//
		// Let X  be the L\times n matrix (L SNPs, n samples) of (mean-centred, scaled) genotypes.  
		// We want the projection of columns of X onto the matrix S whose columns are unit eigenvectors of (1/L) X X^t
		// where L is the number of SNPs.
		// to compute the row of the matrix S of unit eigenvectors of X X^t that corresponds to the current SNP.
		// The matrix S is given by
		//               1 
		//       S = ------- X U D^{-1/2}
		//           \sqrt(L)
		// where
		//       (1/L) X^t X = U D U^t
		// is the eigenvalue decomposition of (1/L) X^t X that we are passed in via set_UDUT (and L is the number of SNPs).
		//
		// The projections are then given by S^t X = \sqrt(L) U D^{1/2}
		// We also wish to compute the correlation between the SNP and the PCA component.
		// With S as above, the PCA components are the projections of columns of X onto columns of S.
		// If we want samples to correspond to columns, this is
		//   S^t X 
		// which can be re-written
		//   sqrt(L) U D^{1/2}
		// i.e. we may as well compute the correlation with columns of U.
		
		m_number_of_PCs_to_compute = std::min( m_number_of_PCs_to_compute, m_number_of_samples ) ;
		m_number_of_PCs_to_compute = std::min( m_number_of_PCs_to_compute, m_number_of_snps ) ;

		// PCAs are given by U D^{1/2}.
		Eigen::VectorXd PCA_eigenvalues = kinship_eigendecomposition.block( 0, 0, m_number_of_PCs_to_compute, 1 ) ;
		Eigen::VectorXd v = PCA_eigenvalues.array().sqrt() ;
		Eigen::MatrixXd PCAs
			= kinship_eigendecomposition.block( 0, 1, m_number_of_samples, m_number_of_PCs_to_compute ) * v.asDiagonal() ;

		send_PCs(
			"Number of SNPs: " + to_string( m_number_of_snps ) + "\n" +
			"Number of samples: " + to_string( m_number_of_samples ) + "\n" +
			"Note: the PCs computed here are 1/√L times the projection of samples onto unit eigenvectors of the variance-covariance matrix\n"
			"    (1/L) X Xᵗ\n"
			"where X is the L x N matrix of genotypes, L is the number of SNPs, and N the number of samples.\n"
			"The constant 1/√L ensures that the PCs do not grow with the number of SNPs.\n"
			"The PCAs are here computed as U D^½ where U and D are the matrices in the UDUᵗ decomposition of the kinship matrix\n"
			"in \"" + m_options.get< std::string >( "-UDUT" ) + "\".\n",
			PCA_eigenvalues,
			PCAs,
			boost::bind(
				&pca::get_concatenated_sample_ids,
				&m_samples,
				_1
			),
			&PCAComputer::get_pca_name
		) ;
	}
	
	m_ui_context.logger() << "========================================================================\n" ;
}

void PCAComputer::send_UDUT_to( UDUTCallback callback ) {
	m_UDUT_signal.connect( callback ) ;
}

void PCAComputer::send_PCs_to( SampleStorage::SharedPtr storage ) {
	m_PC_storage = storage ;
}

void PCAComputer::send_UDUT( std::string description, std::size_t number_of_snps, Eigen::MatrixXd const& UDUT, GetNames row_names, GetNames column_names ) {
	m_UDUT_signal( description, number_of_snps, UDUT, row_names, column_names ) ;
}

void PCAComputer::send_PCs(
	std::string description,
	Eigen::VectorXd const& eigenvalues,
	Eigen::MatrixXd const& PCAs,
	GetNames pca_row_names,
	GetNames pca_column_names
) {
	for( int i = 0; i < PCAs.rows(); ++i ) {
		for( int j = 0; j < PCAs.cols(); ++j ) {
			m_PC_storage->store_per_sample_data(
				"PCAComputer",
				std::size_t(i),
				pca_column_names(j).as< std::string>(),
				description,
				PCAs(i,j)
			) ;
		}
	}
}

