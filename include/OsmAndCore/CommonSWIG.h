#ifndef _OSMAND_CORE_COMMON_SWIG_H_
#define _OSMAND_CORE_COMMON_SWIG_H_

// SWIG_CASTS
#if defined(SWIG)
#   define SWIG_CASTS(thisClass, parentClass)                                                                                   \
        struct Casts                                                                                                            \
        {                                                                                                                       \
            static std::shared_ptr<thisClass> upcastFrom(const std::shared_ptr<parentClass>& input);                            \
            static std::shared_ptr<parentClass> downcastTo_##parentClass(const std::shared_ptr<thisClass>& input);              \
        }
#elif defined(OSMAND_SWIG)
#   define SWIG_CASTS(thisClass, parentClass)                                                                                   \
        struct Casts                                                                                                            \
        {                                                                                                                       \
            static std::shared_ptr<thisClass> upcastFrom(const std::shared_ptr<parentClass>& input)                             \
            {                                                                                                                   \
                return std::dynamic_pointer_cast< thisClass >(input);                                                           \
            }                                                                                                                   \
            static std::shared_ptr<const thisClass> upcastFrom(const std::shared_ptr<const parentClass>& input)                 \
            {                                                                                                                   \
                return std::dynamic_pointer_cast< const thisClass >(input);                                                     \
            }                                                                                                                   \
            static std::shared_ptr<parentClass> downcastTo_##parentClass(const std::shared_ptr<thisClass>& input)               \
            {                                                                                                                   \
                return std::static_pointer_cast< parentClass >(input);                                                          \
            }                                                                                                                   \
            static std::shared_ptr<const parentClass> downcastTo_##parentClass(const std::shared_ptr<const thisClass>& input)   \
            {                                                                                                                   \
                return std::static_pointer_cast< const parentClass >(input);                                                    \
            }                                                                                                                   \
        }
#else
#   define SWIG_CASTS(thisClass, parentClass)
#endif

#if defined(SWIG)
#   define SWIG_DIRECTOR(name) %feature("director") name;
#else
#   define SWIG_DIRECTOR(name)
#endif

#if defined(SWIG)
#   define SWIG_OMIT(x)
#else
#   define SWIG_OMIT(x) x
#endif

#endif // !defined(_OSMAND_CORE_COMMON_SWIG_H_)