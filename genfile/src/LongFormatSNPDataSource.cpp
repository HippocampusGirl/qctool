
//          Copyright Gavin Band 2008 - 2012.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <string>
#include <boost/unordered_map.hpp>
#include <boost/format.hpp>
#include "genfile/FileUtils.hpp"
#include "genfile/SNPDataSource.hpp"
#include "genfile/VariantDataReader.hpp"
#include "genfile/vcf/MetadataParser.hpp"
#include "genfile/LongFormatSNPDataSource.hpp"

namespace genfile {
	namespace {
		bool read_one_line( std::istream& stream, std::string* line ) {
			assert( line ) ;
			std::getline( stream, *line ) ;
			while( stream && (line->size() > 0) && ((*line)[0] == '#') ) {
				std::getline( stream, *line ) ;
			}
			return stream.good() ;
		}
	}
	
	LongFormatSNPDataSource::LongFormatSNPDataSource( std::string const& filename ):
		m_filename( filename ),
		m_buffer_size( 500000000 ),
		m_snp_index(0),
		m_exhausted( false )
	{
		setup( m_filename ) ;
	}

	LongFormatSNPDataSource::LongFormatSNPDataSource( std::istream& stream ):
		m_filename( "(unknown stream)" ),
		m_buffer_size( 500000000 ),
		m_snp_index(0),
		m_exhausted( false )
	{
		setup( stream ) ;
	}

	SNPDataSource::Metadata LongFormatSNPDataSource::get_metadata() const {
		SNPDataSource::Metadata result ;

		{
			std::map< std::string, std::string > format ;
			format[ "ID" ] = "GT" ;
			format[ "Type" ] = "String" ;
			format[ "Number" ] = "1" ;
			format[ "Description" ] = "Genotype" ;
			result.insert( std::make_pair( "FORMAT", format )) ;
		}

		{
			std::map< std::string, std::string > format ;
			format[ "ID" ] = "typed" ;
			format[ "Type" ] = "Integer" ;
			format[ "Number" ] = "1" ;
			format[ "Description" ] = "Was this sample typed at this SNP?" ;
			result.insert( std::make_pair( "FORMAT", format )) ;
		}
		
		return result ;
	}

	void LongFormatSNPDataSource::reset_to_start_impl() {
		m_snp_index = 0 ;
		m_exhausted = false ;
	}
	
	void LongFormatSNPDataSource::get_sample_ids( GetSampleIds setter ) const {
		for( std::size_t i = 0; i < m_samples.size(); ++i ) {
			setter( i, m_samples[i] ) ;
		}
	}
	
	void LongFormatSNPDataSource::get_snp_identifying_data_impl( VariantIdentifyingData* result ) {
		if( m_snp_index == m_variants.size() ) {
			m_exhausted = true ;
			return ;
		} else {
			*result = m_variants[ m_snp_index ] ;
		}
	}

	namespace impl {
		struct LongFormatVariantDataReader: public genfile::VariantDataReader {
			LongFormatVariantDataReader(
				LongFormatSNPDataSource const& source,
				std::vector< std::string > const& samples,
				int snp_index,
				genfile::VariantIdentifyingData const& variant
			) :
				m_source( source ),
				m_samples( samples ),
				m_snp_index( snp_index ),
				m_variant( variant )
			{}
			
			LongFormatVariantDataReader& get( std::string const& spec, PerSampleSetter& setter ) {
				setter.initialise( m_samples.size(), 2 ) ;
				if( spec == "GT" || spec == ":genotypes:" ) {
					for( std::size_t sample_i = 0; sample_i < m_samples.size(); ++sample_i ) {
						setter.set_sample( sample_i ) ;
						std::pair< int, int > snpsample = std::make_pair( m_snp_index, m_source.m_sample_map.at( m_samples[ sample_i ] )) ;
						LongFormatSNPDataSource::BufferMap::const_iterator where = m_source.m_buffer_map.find( snpsample ) ;
						if( where != m_source.m_buffer_map.end() ) {
							m_entry_parser.parse(
								genfile::string_utils::slice( m_source.buffer(), where->second.first, where->second.second ),
								m_variant.number_of_alleles(),
								2, // assume diploid
								setter
							) ;
						} else {
							m_entry_parser.get_missing_value(
								m_variant.number_of_alleles(),
								2,  // assume diploid
								setter
							) ;
						}
					} 
				}
				else if( spec == "typed" ) {
					uint32_t const ploidy = 2 ;
					for( std::size_t sample_i = 0; sample_i < m_samples.size(); ++sample_i ) {
						setter.set_sample( sample_i ) ;
						setter.set_number_of_entries( ploidy, 1, eUnorderedList, eUnknownValueType ) ;
						std::pair< int, int > snpsample = std::make_pair( m_snp_index, m_source.m_sample_map.at( m_samples[ sample_i ] )) ;
						LongFormatSNPDataSource::BufferMap::const_iterator where = m_source.m_buffer_map.find( snpsample ) ;
						setter.set_value( 0, ( where == m_source.m_buffer_map.end() ) ? vcf::EntriesSetter::Integer(0) : vcf::EntriesSetter::Integer(1) ) ;
					}
				} else {
					throw BadArgumentError(
						"genfile::LongFormatVariantDataReader::get()",
						"spec=\"" + spec + "\"",
						"Only \"GT\" and \":genotypes:\" and \"typed\" are supported in a long-format file."
					) ;
				}
				return *this ;
			}

			std::size_t get_number_of_samples() const {
				return m_source.number_of_samples() ;
			}
		
			bool supports( std::string const& spec ) const {
				return spec == "GT" || spec == ":genotypes:" || spec == "typed" ;
			}
		
			void get_supported_specs( SpecSetter setter ) const {
				setter( "GT", "Float" ) ;
				setter( ":genotypes:", "Float" ) ;
				setter( "typed", "Integer" ) ;
			}
		
		private:
			vcf::GenotypeCallVCFEntryType m_entry_parser ;
			LongFormatSNPDataSource const& m_source ;
			std::vector< std::string > const m_samples ;
			int const m_snp_index ;
			genfile::VariantIdentifyingData const& m_variant ;
		} ;
	}

	VariantDataReader::UniquePtr LongFormatSNPDataSource::read_variant_data_impl() {
		std::size_t snp_index = m_snp_index++ ;
		return VariantDataReader::UniquePtr(
			new impl::LongFormatVariantDataReader(
				*this,
				m_samples,
				int( snp_index ),
				m_variants[ snp_index ]
			)
		) ;
	}

	void LongFormatSNPDataSource::ignore_snp_probability_data_impl() {
		++m_snp_index ;
	}

	void LongFormatSNPDataSource::setup( std::string const& filename ) {
		std::auto_ptr< std::istream > stream_ptr = open_text_file_for_input( filename ) ;
		setup( *stream_ptr ) ;
	}
		
	void LongFormatSNPDataSource::setup( std::istream& stream ) {
		// read header line
		using namespace genfile::string_utils ;
		std::string line ;
		std::vector< slice > elts ;
		
		std::vector< VariantEntry > expectedColumns ;
		expectedColumns.push_back( "SNPID" ) ;
		expectedColumns.push_back( "rsid" ) ;
		expectedColumns.push_back( "chromosome" ) ;
		expectedColumns.push_back( "position" ) ;
		expectedColumns.push_back( "numberOfAlleles" ) ;
		expectedColumns.push_back( "allele1" ) ;
		expectedColumns.push_back( "other_alleles" ) ;
		expectedColumns.push_back( MissingValue() ) ;
		expectedColumns.push_back( "ploidy" ) ;
		expectedColumns.push_back( "genotype" ) ;
		
		// Read and validate header line
		{
			if( !read_one_line( stream, &line ) ) {
				throw genfile::MalformedInputError(
					m_filename,
					"Header line not found",
					0
				) ;
			}
			elts = slice( line ).split( " \t," ) ;
		
			if( elts.size() < expectedColumns.size() ) {
				throw genfile::MalformedInputError(
					m_filename,
					( boost::format( "Expected at least %d header columns" ) % expectedColumns.size() ).str(),
					0
				) ;
			}

			for( std::size_t i = 0; i < expectedColumns.size(); ++i ) {
				if( !expectedColumns[i].is_missing() && elts[i] != expectedColumns[i].as< std::string >() ) {
					throw genfile::MalformedInputError(
						m_filename,
						( boost::format( "Expected column name \"%s\"" ) % expectedColumns[i].as< std::string >() ).str(),
						0, i
					) ;
				}
			}
		}
		
		// Read and validate SNPs and samples
		m_buffer.resize( m_buffer_size ) ;
		{
			VariantIdentifyingData snp ;
			std::size_t lineNumber = 0 ;

			std::size_t buffer_pos = 0 ;

			while( read_one_line( stream, &line )) {
				++lineNumber ;
				elts = slice( line ).split( " \t," ) ;
				if( elts.size() < expectedColumns.size() ) {
					throw genfile::MalformedInputError(
						m_filename,
						( boost::format( "Wrong number of entries (%d, expected at least %d)" ) % elts.size() % expectedColumns.size() ).str(),
						lineNumber
					) ;
				}
			
				if( to_repr< int >( elts[4] ) < 2 ) {
					throw genfile::MalformedInputError(
						m_filename,
						"Expected at least two alleles.",
						lineNumber, 3
					) ;
				}

				if( elts[8] != "2" ) {
					throw genfile::MalformedInputError(
						m_filename,
						"Only diploid samples are currently supported.",
						lineNumber, 7
					) ;
				}
				
				// parse alternate alleles
				std::vector< slice > alternate_alleles = elts[6].split( "," ) ;
				if( alternate_alleles.size() + 1 != to_repr< int >( elts[4] )) {
					throw genfile::MalformedInputError(
						m_filename,
						"Wrong number of alleles found (" + to_string( alternate_alleles.size() + 1 ) + ", expected " + elts[4],
						lineNumber, 5
					) ;
				}

				VariantIdentifyingData snp(
					elts[0], elts[1],
					GenomePosition(
						Chromosome( elts[2] ), to_repr< Position >( elts[3] )
					),
					elts[5], alternate_alleles[0]
				) ;
				for( std::size_t ai = 1; ai < alternate_alleles.size(); ++ ai ) {
					snp.add_allele( alternate_alleles[ai] ) ;
				}
				
				int snpIndex = -1 ;
				{
					VariantMap::const_iterator where = m_variant_map.find( snp ) ;
					if( where != m_variant_map.end() ) {
						snpIndex = where->second ;
					} else {
						m_variants.push_back( snp ) ;
						if( !m_variant_map.insert( VariantMap::value_type( snp, m_variants.size() - 1 ) ).second ) {
							throw genfile::OperationFailedError(
								"genfile::LongFormatSNPDataSource::setup()",
								"m_variant_map",
								"insert SNP " + to_string( snp )
							) ;
						}
						snpIndex = m_variants.size() - 1 ;
					}
				}
				assert( snpIndex >= 0 ) ;

				int sampleIndex = -1 ;
				{
					// Store the sample
					SampleMap::const_iterator where = m_sample_map.find( elts[7] ) ;

					if( where != m_sample_map.end() ) {
						sampleIndex = where->second ;
					}
					else {
						sampleIndex = m_samples.size() ;
						m_samples.push_back( elts[7] ) ;
						if( !m_sample_map.insert( SampleMap::value_type( elts[7], sampleIndex )).second ) {
							throw genfile::OperationFailedError(
								"genfile::LongFormatSNPDataSource::setup()",
								"m_sample_map",
								"insert sample " + std::string( elts[7] )
							) ;
						}
					}
				}
				assert( sampleIndex >= 0 ) ;

				// Store the genotype
				// Store the index of the genotype
				std::size_t next_buffer_pos = buffer_pos + elts[9].size() ;
				assert( next_buffer_pos <= m_buffer.size() ) ;
				m_buffer_map.insert( BufferMap::value_type( std::make_pair( snpIndex, sampleIndex ), std::make_pair( buffer_pos, next_buffer_pos ) )) ;
				std::copy( elts[9].begin(), elts[9].end(), &m_buffer[0] + buffer_pos ) ;
				buffer_pos = next_buffer_pos ;
			}
		}
		assert( m_variants.size() == m_variant_map.size() ) ;
		assert( m_samples.size() == m_sample_map.size() ) ;
	}

}

