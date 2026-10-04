#ifndef __QDMA_P2PDMA_H__
#define __QDMA_P2PDMA_H__

#include <linux/pci.h>

int qdma_p2pdma_init(void);

void qdma_p2pdma_exit(void);

int qdma_p2pdma_probe(struct pci_dev *pdev, int bar);

void qdma_p2pdma_remove(struct pci_dev *pdev);

#endif /* __QDMA_P2PDMA_H__ */
