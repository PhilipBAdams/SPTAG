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

            void ReconstructVector(const std::uint8_t* qvec, void* vecout);

            virtual std::uint64_t BufferSize() const;

            virtual ErrorCode SaveQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_out) const;

            virtual ErrorCode LoadQuantizer(std::shared_ptr<Helper::DiskPriorityIO> p_in);

            virtual DimensionType GetNumSubvectors() const;

            SizeType GetKsPerSubvector() const;

            DimensionType GetDimPerSubvector() const;

            bool GetEnableADC() const;

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

            inline SizeType m_DistIndexCalc(SizeType i, SizeType j, SizeType k);

            std::unique_ptr<T[]> m_codebooks;
            std::unique_ptr<float[]> m_CosineDistanceTables;
            std::unique_ptr<float[]> m_L2DistanceTables;
        };

        template <typename T>
        PQQuantizer<T>::PQQuantizer()
        {
        }

        template <typename T>
        PQQuantizer<T>::PQQuantizer(DimensionType NumSubvectors, SizeType KsPerSubvector, DimensionType DimPerSubvector, bool EnableADC, T* Codebooks)
        {
            m_NumSubvectors = NumSubvectors;
            m_KsPerSubvector = KsPerSubvector;
            m_DimPerSubvector = DimPerSubvector;
            m_codebooks.reset(Codebooks);
            m_EnableADC = EnableADC;

            m_BlockSize = (m_KsPerSubvector * (m_KsPerSubvector + 1)) / 2;
            m_CosineDistanceTables.reset(new float[m_BlockSize * m_NumSubvectors]);
            m_L2DistanceTables.reset(new float[m_BlockSize * m_NumSubvectors]);

            auto cosineDist = DistanceCalcSelector<T>(DistCalcMethod::Cosine);
            auto L2Dist = DistanceCalcSelector<T>(DistCalcMethod::L2);

            for (int i = 0; i < m_NumSubvectors; i++) {
                T* base = m_codebooks.get() + i * m_KsPerSubvector * m_DimPerSubvector;
                for (int j = 0; j < m_KsPerSubvector; j++) {
                    for (int k = 0; k <= j; k++) {
                        m_CosineDistanceTables[m_DistIndexCalc(i, j, k)] = DistanceUtils::ConvertDistanceBackToCosineSimilarity(cosineDist(base + j * m_DimPerSubvector, base + k * m_DimPerSubvector, m_DimPerSubvector));
                        m_L2DistanceTables[m_DistIndexCalc(i, j, k)] = L2Dist(base + j * m_DimPerSubvector, base + k * m_DimPerSubvector, m_DimPerSubvector);
                    }
                }
            }
        }

        template <typename T>
        PQQuantizer<T>::~PQQuantizer()
        {
        }

        template <typename T>
        float PQQuantizer<T>::L2Distance(const std::uint8_t* pX, const std::uint8_t* pY)
        {
            if (GetEnableADC() == false) {
                float out = 0;
                for (int i = 0; i < m_NumSubvectors; i++) {
                    out += m_L2DistanceTables[m_DistIndexCalc(i, pX[i], pY[i])];
                }
                return out;
            }
            else {
                float out = 0;
                for (int i = 0; i < m_NumSubvectors; i++) {
                    out += pY[i * m_KsPerSubvector + pX[i]] * pY[i * m_KsPerSubvector + pX[i]];
                }
                return out;
            }
        }

        template <typename T>
        float PQQuantizer<T>::CosineDistance(const std::uint8_t* pX, const std::uint8_t* pY)
        {
            float out = 0;
            for (int i = 0; i < m_NumSubvectors; i++) {
                out += m_CosineDistanceTables[m_DistIndexCalc(i, pX[i], pY[i])];
            }
            return DistanceUtils::ConvertCosineSimilarityToDistance(out);
        }

        template <typename T>
        void PQQuantizer<T>::QuantizeVector(const void* vec, std::uint8_t* vecout)
        {
            auto distCalc = DistanceCalcSelector<T>(DistCalcMethod::L2);

            for (int i = 0; i < m_NumSubvectors; i++) {
                SizeType bestIndex = 0;
                float minDist = std::numeric_limits<float>::infinity();

                const T* subvec = ((T*)vec) + i * m_DimPerSubvector;
                T* basevec = m_codebooks.get() + i * m_KsPerSubvector * m_DimPerSubvector;
                for (int j = 0; j < m_KsPerSubvector; j++) {
                    float dist = distCalc(subvec, basevec + j * m_DimPerSubvector, m_DimPerSubvector);
                    if (dist < minDist) {
                        bestIndex = j;
                        minDist = dist;
                    }
                }
                vecout[i] = bestIndex;
            }
        }

        template <typename T>
        void PQQuantizer<T>::ReconstructVector(const std::uint8_t* qvec, void* vecout)
        {
            for (int i = 0; i < m_NumSubvectors; i++) {
                T* codebook_entry = m_codebooks.get() + (i * m_KsPerSubvector * m_DimPerSubvector) + (qvec[i] * m_DimPerSubvector);
                T* sub_vecout = &((T*)vecout)[i * m_DimPerSubvector];
                for (int j = 0; j < m_DimPerSubvector; j++) {
                    sub_vecout[j] = codebook_entry[j];
                }
            }
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
            IOBINARY(p_in, ReadBinary, sizeof(DimensionType), (char*)&m_NumSubvectors);
            IOBINARY(p_in, ReadBinary, sizeof(SizeType), (char*)&m_KsPerSubvector);
            IOBINARY(p_in, ReadBinary, sizeof(DimensionType), (char*)&m_DimPerSubvector);
            m_codebooks.reset(new T[m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector]);
            IOBINARY(p_in, ReadBinary, sizeof(T) * m_NumSubvectors * m_KsPerSubvector * m_DimPerSubvector, (char*)m_codebooks.get());

            m_BlockSize = (m_KsPerSubvector * (m_KsPerSubvector + 1)) / 2;
            m_CosineDistanceTables.reset(new float[m_BlockSize * m_NumSubvectors]);
            m_L2DistanceTables.reset(new float[m_BlockSize * m_NumSubvectors]);

            auto cosineDist = DistanceCalcSelector<T>(DistCalcMethod::Cosine);
            auto L2Dist = DistanceCalcSelector<T>(DistCalcMethod::L2);

            for (int i = 0; i < m_NumSubvectors; i++) {
                T* base = m_codebooks.get() + i * m_KsPerSubvector * m_DimPerSubvector;
                for (int j = 0; j < m_KsPerSubvector; j++) {
                    for (int k = 0; k <= j; k++) {
                        m_CosineDistanceTables[m_DistIndexCalc(i, j, k)] = DistanceUtils::ConvertDistanceBackToCosineSimilarity(cosineDist(base + j * m_DimPerSubvector, base + k * m_DimPerSubvector, m_DimPerSubvector));
                        m_L2DistanceTables[m_DistIndexCalc(i, j, k)] = L2Dist(base + j * m_DimPerSubvector, base + k * m_DimPerSubvector, m_DimPerSubvector);
                    }
                }
            }
            LOG(Helper::LogLevel::LL_Info, "Load quantizer: Subvectors:%d KsPerSubvector:%d DimPerSubvector:%d\n", m_NumSubvectors, m_KsPerSubvector, m_DimPerSubvector);
            return ErrorCode::Success;
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
        bool PQQuantizer<T>::GetEnableADC() const
        {
            return m_EnableADC;
        }

        template <typename T>
        inline SizeType PQQuantizer<T>::m_DistIndexCalc(SizeType i, SizeType j, SizeType k) {
            if (k > j) {
                return (m_BlockSize * i) + ((k * (k + 1)) / 2) + j; // exploit symmetry by swapping
            }
            return (m_BlockSize * i) + ((j * (j + 1)) / 2) + k;
        }
    }
}

#endif // _SPTAG_COMMON_PQQUANTIZER_H_