// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_COMMON_PQQUANTIZER_H_
#define _SPTAG_COMMON_PQQUANTIZER_H_

#include "CommonUtils.h"
#include "DistanceUtils.h"
#include "Quantizer.h"
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <cassert>
#include <immintrin.h>


namespace SPTAG
{
    namespace COMMON
    {
        template <typename T>
        class PQQuantizer : public Quantizer
        {
        public:
            PQQuantizer();

            PQQuantizer(DimensionType NumSubvectors, SizeType KsPerSubvector, DimensionType DimPerSubvector, bool EnableADC, T* Codebooks);

            ~PQQuantizer();

            virtual float L2Distance(const std::uint8_t* pX, const std::uint8_t* pY);

            virtual float CosineDistance(const std::uint8_t* pX, const std::uint8_t* pY);

            virtual void QuantizeVector(const void* vec, std::uint8_t* vecout);
            
            virtual SizeType QuantizeSize();

            void ReconstructVector(const std::uint8_t* qvec, void* vecout);

            virtual SizeType ReconstructSize();

            virtual DimensionType ReconstructDim();

            virtual std::uint64_t BufferSize() const;

            virtual ErrorCode SaveQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_out) const;

            virtual ErrorCode LoadQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_in);

            virtual DimensionType GetNumSubvectors() const;

            virtual float GetBase();

            SizeType GetKsPerSubvector() const;

            DimensionType GetDimPerSubvector() const;

            virtual bool GetEnableADC();

            virtual void SetEnableADC(bool enableADC);

            VectorValueType GetReconstructType()
            {
                return GetEnumValueType<T>();
            }

            QuantizerType GetQuantizerType() {
                return QuantizerType::PQQuantizer;
            }

        private:
            DimensionType m_NumSubvectors;
            SizeType m_KsPerSubvector;
            DimensionType m_DimPerSubvector;
            SizeType m_BlockSize;
            bool m_EnableADC;
            //bool m_IsSearching;

            inline SizeType m_DistIndexCalc(SizeType i, SizeType j, SizeType k);

            std::unique_ptr<T[]> m_codebooks;
            std::unique_ptr<const float[]> m_CosineDistanceTables;
            std::unique_ptr<const float[]> m_L2DistanceTables;
            const __m256i m_offset = _mm256_set_epi32( 0, 1 * 256 * 256, 2 * 256 * 256, 3 * 256 * 256, 4 * 256 * 256, 5 * 256 * 256, 6 * 256 * 256, 7 * 256 * 256 );
            const __m256i m_toMul = _mm256_set1_epi32(256);

        };

        template <typename T>
        PQQuantizer<T>::PQQuantizer()
        {
        }

        template <typename T>
        PQQuantizer<T>::PQQuantizer(DimensionType NumSubvectors, SizeType KsPerSubvector, DimensionType DimPerSubvector, bool EnableADC, T* Codebooks) : m_NumSubvectors(NumSubvectors), m_KsPerSubvector(KsPerSubvector), m_DimPerSubvector(DimPerSubvector), m_BlockSize(KsPerSubvector* KsPerSubvector)
        {
            m_codebooks.reset(Codebooks);
            m_EnableADC = EnableADC;

            auto temp_m_CosineDistanceTables = std::make_unique<float[]>(m_BlockSize * m_NumSubvectors);
            auto temp_m_L2DistanceTables = std::make_unique<float[]>(m_BlockSize * m_NumSubvectors);

            auto cosineDist = DistanceCalcSelector<T>(DistCalcMethod::Cosine);
            auto L2Dist = DistanceCalcSelector<T>(DistCalcMethod::L2);

            for (int i = 0; i < m_NumSubvectors; i++) {
                SizeType baseIdx = i * m_KsPerSubvector * m_DimPerSubvector;
                for (int j = 0; j < m_KsPerSubvector; j++) {
                    for (int k = 0; k < m_KsPerSubvector; k++) {
                        temp_m_CosineDistanceTables[m_DistIndexCalc(i, j, k)] = DistanceUtils::ConvertDistanceBackToCosineSimilarity(cosineDist(&m_codebooks[baseIdx + j * m_DimPerSubvector], &m_codebooks[baseIdx + k * m_DimPerSubvector], m_DimPerSubvector));
                        temp_m_L2DistanceTables[m_DistIndexCalc(i, j, k)] = L2Dist(&m_codebooks[baseIdx + j * m_DimPerSubvector], &m_codebooks[baseIdx + k * m_DimPerSubvector], m_DimPerSubvector);
                    }
                }
            }
            m_CosineDistanceTables = std::move(temp_m_CosineDistanceTables);
            m_L2DistanceTables = std::move(temp_m_L2DistanceTables);
        }

        template <typename T>
        PQQuantizer<T>::~PQQuantizer()
        {
        }

        template <typename T>
        float PQQuantizer<T>::L2Distance(const std::uint8_t* pX, const std::uint8_t* pY)
            // pX must be query distance table for ADC
        {
            float out = 0;
            if (GetEnableADC()) {               
                for (int i = 0; i < m_NumSubvectors; i++) {
                    out += ((float*) pX)[i * m_KsPerSubvector + (size_t) pY[i]];
                }
            }
            else {
                for (int i = 0; i < m_NumSubvectors/8; i++) {
                    __m128i loadpX = _mm_set_epi64x(*((long long*)pX + i * 8), *((long long*)pX + i * 8));
                    __m256i working = _mm256_cvtepu8_epi32(loadpX);
                    working = _mm256_mullo_epi32(working, m_toMul);
                    working = _mm256_add_epi32(working, m_offset);
                    __m128i loadpY = _mm_set_epi64x(*((long long*)pY + i * 8), *((long long*)pY + i * 8));
                    __m256i toAdd = _mm256_cvtepu8_epi32(loadpY);
                    working = _mm256_add_epi32(working, toAdd);
                    working = _mm256_i32gather_epi32((const int*)m_L2DistanceTables.get() + (i * 8 * 256 * 256), working, 1);
                    __m256 toFloat = _mm256_cvtepi32_ps(working);
                    float arr[8];
                    _mm256_store_ps(arr, toFloat);
                    out = arr[0] + arr[1] + arr[2] + arr[3] + arr[4] + arr[5] + arr[6] + arr[7];
                }                
            }
            return out;
        }

        template <typename T>
        float PQQuantizer<T>::CosineDistance(const std::uint8_t* pX, const std::uint8_t* pY)
            // pX must be query distance table for ADC
        {
            float out = 0;
            if (GetEnableADC())
            {
                for (int i = 0; i < m_NumSubvectors; i++) {
                    out += ((float*)pX)[(m_NumSubvectors * m_KsPerSubvector) + (i * m_KsPerSubvector) + (size_t) pY[i]];
                }
            }
            else
            {
                for (int i = 0; i < m_NumSubvectors; i++) {
                    out += m_CosineDistanceTables[m_DistIndexCalc(i, pX[i], pY[i])];
                }
            }
            return DistanceUtils::ConvertCosineSimilarityToDistance(out);
        }

        template <typename T>
        void PQQuantizer<T>::QuantizeVector(const void* vec, std::uint8_t* vecout)
        {
            if (GetEnableADC())
            {
                auto distCalcL2 = DistanceCalcSelector<T>(DistCalcMethod::L2);
                auto distCalcCosine = DistanceCalcSelector<T>(DistCalcMethod::L2);
                float* ADCtable = (float*) vecout;

                for (int i = 0; i < m_NumSubvectors; i++)
                {
                    const T* subvec = ((T*)vec) + i * m_DimPerSubvector;
                    SizeType basevecIdx = i * m_KsPerSubvector * m_DimPerSubvector;
                    for (int j = 0; j < m_KsPerSubvector; j++)
                    {
                        ADCtable[i * m_KsPerSubvector + j] = distCalcL2(subvec, &m_codebooks[basevecIdx + j * m_DimPerSubvector], m_DimPerSubvector);
                        ADCtable[(m_NumSubvectors * m_KsPerSubvector) + i * m_KsPerSubvector + j] = distCalcCosine(subvec, &m_codebooks[basevecIdx + j * m_DimPerSubvector], m_DimPerSubvector);
                    }
                }
            }
            else 
            {
                auto distCalc = DistanceCalcSelector<T>(DistCalcMethod::L2);

                for (int i = 0; i < m_NumSubvectors; i++) {
                    int bestIndex = -1;
                    float minDist = std::numeric_limits<float>::infinity();

                    const T* subvec = ((T*)vec) + i * m_DimPerSubvector;
                    SizeType basevecIdx = i * m_KsPerSubvector * m_DimPerSubvector;
                    for (int j = 0; j < m_KsPerSubvector; j++) {
                        float dist = distCalc(subvec, &m_codebooks[basevecIdx + j * m_DimPerSubvector], m_DimPerSubvector);
                        if (dist < minDist) {
                            bestIndex = j;
                            minDist = dist;
                        }
                    }
                    assert(bestIndex != -1);
                    vecout[i] = bestIndex;
                }
            }           
        }

        template <typename T>
        SizeType PQQuantizer<T>::QuantizeSize()
        {
            if (GetEnableADC())
            {
                return sizeof(float) * m_NumSubvectors * m_KsPerSubvector * 2;
            }
            else
            {
                return m_NumSubvectors;
            }           
        }

        template <typename T>
        void PQQuantizer<T>::ReconstructVector(const std::uint8_t* qvec, void* vecout)
        {
            for (int i = 0; i < m_NumSubvectors; i++) {
                SizeType codebook_idx = (i * m_KsPerSubvector * m_DimPerSubvector) + (qvec[i] * m_DimPerSubvector);
                T* sub_vecout = &((T*)vecout)[i * m_DimPerSubvector];
                for (int j = 0; j < m_DimPerSubvector; j++) {
                    sub_vecout[j] = m_codebooks[codebook_idx + j];
                }
            }
        }

        template <typename T>
        SizeType PQQuantizer<T>::ReconstructSize()
        {       
            return sizeof(T) * ReconstructDim();
        }

        template <typename T>
        DimensionType PQQuantizer<T>::ReconstructDim()
        {
            return m_DimPerSubvector * m_NumSubvectors;
        }

        template <typename T>
        std::uint64_t PQQuantizer<T>::BufferSize() const
        {
            return sizeof(T) * m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector + sizeof(DimensionType) + sizeof(SizeType) + sizeof(DimensionType);
        }

        template <typename T>
        ErrorCode PQQuantizer<T>::SaveQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_out) const
        {
            IOBINARY(p_out, WriteBinary, sizeof(DimensionType), (char*)&m_NumSubvectors);
            IOBINARY(p_out, WriteBinary, sizeof(SizeType), (char*)&m_KsPerSubvector);
            IOBINARY(p_out, WriteBinary, sizeof(DimensionType), (char*)&m_DimPerSubvector);
            IOBINARY(p_out, WriteBinary, sizeof(T) * m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector, (char*)m_codebooks.get());
            LOG(Helper::LogLevel::LL_Info, "Saving quantizer: Subvectors:%d KsPerSubvector:%d DimPerSubvector:%d\n", m_NumSubvectors, m_KsPerSubvector, m_DimPerSubvector);
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode PQQuantizer<T>::LoadQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_in)
        {
            LOG(Helper::LogLevel::LL_Info, "Loading Quantizer.\n");
            IOBINARY(p_in, ReadBinary, sizeof(DimensionType), (char*)&m_NumSubvectors);
            LOG(Helper::LogLevel::LL_Info, "After read subvecs: %s.\n", std::to_string(m_NumSubvectors).c_str());
            IOBINARY(p_in, ReadBinary, sizeof(SizeType), (char*)&m_KsPerSubvector);
            LOG(Helper::LogLevel::LL_Info, "After read ks: %s.\n", std::to_string(m_KsPerSubvector).c_str());
            IOBINARY(p_in, ReadBinary, sizeof(DimensionType), (char*)&m_DimPerSubvector);
            LOG(Helper::LogLevel::LL_Info, "After read dim: %s.\n", std::to_string(m_DimPerSubvector).c_str());
            m_codebooks = std::make_unique<T[]>(m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector);
            LOG(Helper::LogLevel::LL_Info, "sizeof(T): %s.\n", std::to_string(sizeof(T)).c_str());
            IOBINARY(p_in, ReadBinary, sizeof(T) * m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector, (char*)m_codebooks.get());
            LOG(Helper::LogLevel::LL_Info, "After read codebooks.\n");

            m_BlockSize = m_KsPerSubvector * m_KsPerSubvector;
            auto temp_m_CosineDistanceTables = std::make_unique<float[]>(m_BlockSize * m_NumSubvectors);
            auto temp_m_L2DistanceTables = std::make_unique<float[]>(m_BlockSize * m_NumSubvectors);

            auto cosineDist = DistanceCalcSelector<T>(DistCalcMethod::Cosine);
            auto L2Dist = DistanceCalcSelector<T>(DistCalcMethod::L2);

            for (int i = 0; i < m_NumSubvectors; i++) {
                SizeType baseIdx = i * m_KsPerSubvector * m_DimPerSubvector;
                for (int j = 0; j < m_KsPerSubvector; j++) {
                    for (int k = 0; k < m_KsPerSubvector; k++) {
                        temp_m_CosineDistanceTables[m_DistIndexCalc(i, j, k)] = DistanceUtils::ConvertDistanceBackToCosineSimilarity(cosineDist(&m_codebooks[baseIdx + j * m_DimPerSubvector], &m_codebooks[baseIdx + k * m_DimPerSubvector], m_DimPerSubvector));
                        temp_m_L2DistanceTables[m_DistIndexCalc(i, j, k)] = L2Dist(&m_codebooks[baseIdx + j * m_DimPerSubvector], &m_codebooks[baseIdx + k * m_DimPerSubvector], m_DimPerSubvector);
                    }
                }
            }
            m_CosineDistanceTables = std::move(temp_m_CosineDistanceTables);
            m_L2DistanceTables = std::move(temp_m_L2DistanceTables);
            LOG(Helper::LogLevel::LL_Info, "Loaded quantizer: Subvectors:%d KsPerSubvector:%d DimPerSubvector:%d\n", m_NumSubvectors, m_KsPerSubvector, m_DimPerSubvector);
            return ErrorCode::Success;
        }

        template <typename T>
        float PQQuantizer<T>::GetBase()
        {
            return COMMON::Utils::GetBase<T>();
        }

        template <typename T>
        DimensionType PQQuantizer<T>::GetNumSubvectors() const
        {
            return m_NumSubvectors;
        }

        template <typename T>
        SizeType PQQuantizer<T>::GetKsPerSubvector() const
        {
            return m_KsPerSubvector;
        }

        template <typename T>
        DimensionType PQQuantizer<T>::GetDimPerSubvector() const
        {
            return m_DimPerSubvector;
        }

        template <typename T>
        bool PQQuantizer<T>::GetEnableADC()
        {
            return m_EnableADC;
        }

        template <typename T>
        void PQQuantizer<T>::SetEnableADC(bool enableADC)
        {
            m_EnableADC = enableADC;
        }

        template <typename T>
        inline SizeType PQQuantizer<T>::m_DistIndexCalc(SizeType i, SizeType j, SizeType k) {
            return m_BlockSize * i + j * m_KsPerSubvector + k;
        }
    }
}

#endif // _SPTAG_COMMON_PQQUANTIZER_H_